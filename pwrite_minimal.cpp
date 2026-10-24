// Minimal repro: two disjoint pwrites sharing one unwritten 4 KiB block.
//
// The file is sized with ftruncate and never written before, so every block
// starts sparse. Each trial uses a fresh region of it:
//   A: one pwrite of A_BYTES of 0xAA, ending SPLIT bytes into a 4 KiB block
//   B: one pwrite of B_BYTES of 0xBB, starting right where A ends
// so A and B are disjoint but share that block. Then the shared block and
// B's bytes are read back with pread and checked.
//
// ORDER says when B runs:
//   inside  B starts DELAY_MS after A starts (A still running)
//   before  B runs and returns before A starts
//   after   B starts after A returns
// Each trial records whether B really ran entirely inside A's call.
//
// PWRITE_MINIMAL_B_AFTER says what B does once its pwrite returns, to push
// the shared block's page out of the cache before A reaches it:
//   none     nothing (default)
//   msync    msync(MS_SYNC | MS_INVALIDATE) on a mapping of the shared page
//   fsync    fsync the file
//   nocache  B writes through a second descriptor with F_NOCACHE instead
//
// PWRITE_MINIMAL_PREFILL=1 writes zeros over the whole file before the
// trials, so no block is sparse.
//
// usage: pwrite_minimal PATH TRIALS A_MB SPLIT B_BYTES ORDER [DELAY_MS]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

typedef std::chrono::steady_clock clock_type;

static void die(const char *what)
{
  perror(what);
  exit(2);
}

// One pwrite call, which must write every byte.
static void pwrite_once(int fd, const std::vector<unsigned char>& buf, size_t offset)
{
  ssize_t r = pwrite(fd, buf.data(), buf.size(), off_t(offset));
  if(r < 0)
    {
      die("pwrite");
    }
  if(size_t(r) != buf.size())
    {
      fprintf(stderr, "pwrite of %zu bytes at %zu wrote %zd\n", buf.size(), offset, r);
      exit(2);
    }
}

int main(int argc, char **argv)
{
  if(argc < 7)
    {
      fprintf(stderr, "usage: pwrite_minimal PATH TRIALS A_MB SPLIT B_BYTES inside|before|after [DELAY_MS]\n");
      return 2;
    }
  std::string path = argv[1];
  size_t trials = size_t(atol(argv[2]));
  size_t a_bytes = size_t(atof(argv[3]) * 1048576.0);
  size_t split = size_t(atol(argv[4]));
  size_t b_bytes = size_t(atol(argv[5]));
  std::string order = argv[6];
  double delay_ms = argc > 7 ? atof(argv[7]) : 5;
  if(split >= 4096 || (order != "inside" && order != "before" && order != "after"))
    {
      fprintf(stderr, "SPLIT must be below 4096, ORDER inside, before or after\n");
      return 2;
    }

  // Each trial's region starts on a 1 MiB boundary; A's start is placed so
  // that A ends split bytes past a 4 KiB boundary.
  size_t a_rounded = (a_bytes + 4095) / 4096 * 4096;
  size_t stride = (a_rounded + b_bytes + 2 * 1048576) / 1048576 * 1048576;
  size_t total = trials * stride;

  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  if(fd < 0)
    {
      die("open");
    }
  if(ftruncate(fd, off_t(total)) != 0)
    {
      die("ftruncate");
    }

  bool prefill = getenv("PWRITE_MINIMAL_PREFILL") && std::string(getenv("PWRITE_MINIMAL_PREFILL")) == "1";
  if(prefill)
    {
      std::vector<unsigned char> zeros(size_t(1) << 26, 0);
      for(size_t off = 0; off < total; off += zeros.size())
        {
          size_t n = std::min(zeros.size(), total - off);
          if(pwrite(fd, zeros.data(), n, off_t(off)) != ssize_t(n))
            {
              die("prefill pwrite");
            }
        }
      if(fsync(fd) != 0)
        {
          die("prefill fsync");
        }
    }
  std::string b_after = getenv("PWRITE_MINIMAL_B_AFTER") ? getenv("PWRITE_MINIMAL_B_AFTER") : "none";
  if(b_after != "none" && b_after != "msync" && b_after != "fsync" && b_after != "nocache")
    {
      fprintf(stderr, "PWRITE_MINIMAL_B_AFTER must be none, msync, fsync or nocache\n");
      return 2;
    }
  int b_fd = fd;
  if(b_after == "nocache")
    {
      b_fd = ::open(path.c_str(), O_RDWR);
      if(b_fd < 0 || fcntl(b_fd, F_NOCACHE, 1) != 0)
        {
          die("open nocache");
        }
    }
  long page = sysconf(_SC_PAGESIZE);
  std::vector<unsigned char> a_buf(a_bytes, 0xAA), b_buf(b_bytes, 0xBB), check(b_bytes + 4096);
  size_t lost_trials = 0, lost_bytes = 0, inside_trials = 0, inside_lost = 0;
  double a_ms_sum = 0;
  for(size_t t = 0; t < trials; ++t)
    {
      size_t shared = t * stride + a_rounded;  // the shared 4 KiB block
      size_t x = shared + split;              // A ends and B starts here
      size_t a_off = x - a_bytes;

      std::atomic<bool> a_started{false};
      clock_type::time_point a0, a1, b0, b1;
      auto run_a = [&]()
      {
        a0 = clock_type::now();
        a_started.store(true);
        pwrite_once(fd, a_buf, a_off);
        a1 = clock_type::now();
      };
      auto run_b = [&]()
      {
        b0 = clock_type::now();
        pwrite_once(b_fd, b_buf, x);
        b1 = clock_type::now();
        if(b_after == "msync")
          {
            size_t map_off = x / size_t(page) * size_t(page);
            void *m = mmap(nullptr, size_t(page), PROT_READ, MAP_SHARED, fd, off_t(map_off));
            if(m == MAP_FAILED)
              {
                die("mmap");
              }
            if(msync(m, size_t(page), MS_SYNC | MS_INVALIDATE) != 0)
              {
                die("msync");
              }
            if(munmap(m, size_t(page)) != 0)
              {
                die("munmap");
              }
          }
        else if(b_after == "fsync")
          {
            if(fsync(fd) != 0)
              {
                die("fsync");
              }
          }
      };

      if(order == "before")
        {
          run_b();
          run_a();
        }
      else if(order == "after")
        {
          run_a();
          run_b();
        }
      else
        {
          std::thread ta(run_a);
          while(!a_started.load())
            {
            }
          std::this_thread::sleep_for(std::chrono::microseconds(int64_t(delay_ms * 1000)));
          std::thread tb(run_b);
          ta.join();
          tb.join();
        }
      bool inside = a0 <= b0 && b1 < a1;
      inside_trials += inside;
      a_ms_sum += std::chrono::duration<double, std::milli>(a1 - a0).count();

      // Read back the shared block from its start through B's end.
      size_t want = split + b_bytes;
      check.assign(want, 0);
      ssize_t r = pread(fd, check.data(), want, off_t(shared));
      if(r < 0 || size_t(r) != want)
        {
          die("pread");
        }
      size_t a_bad = 0, b_bad = 0, first_bad = SIZE_MAX, last_bad = 0;
      for(size_t i = 0; i < want; ++i)
        {
          unsigned char expect = (i < split) ? 0xAA : 0xBB;
          if(check[i] != expect)
            {
              (i < split ? a_bad : b_bad) += 1;
              first_bad = std::min(first_bad, i);
              last_bad = i;
            }
        }
      if(a_bad || b_bad)
        {
          ++lost_trials;
          lost_bytes += a_bad + b_bad;
          inside_lost += inside;
          if(lost_trials <= 20)
            {
              printf("  trial %zu: shared block at %zu, A lost %zu bytes, B lost %zu bytes, bad bytes [%zu, %zu) of the block; A %.1f ms, B started %.1f ms after A and %s\n",
                     t, shared, a_bad, b_bad, first_bad, last_bad + 1,
                     std::chrono::duration<double, std::milli>(a1 - a0).count(),
                     std::chrono::duration<double, std::milli>(b0 - a0).count(),
                     inside ? "returned before A" : "did not run inside A");
            }
        }
    }
  ::close(fd);
  ::unlink(path.c_str());
  printf("%sB after: %s; ", prefill ? "prefilled, " : "", b_after.c_str());
  printf("%s, A %.1f MiB ending %zu bytes into a block, B %zu bytes, delay %.1f ms: %zu trials, %zu with B inside A, A %.1f ms on average; %zu trials lost data (%zu of them B inside A), %zu bytes lost\n",
         order.c_str(), double(a_bytes) / 1048576.0, split, b_bytes, delay_ms, trials, inside_trials, a_ms_sum / double(trials), lost_trials, inside_lost, lost_bytes);
  return lost_trials ? 1 : 0;
}
