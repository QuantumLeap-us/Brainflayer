/* Copyright (c) 2015 Ryan Castellucci, All Rights Reserved */
#include <time.h>
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

#include <openssl/sha.h>

#include <sys/time.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/sysinfo.h>

#include <curl/curl.h>
#include <pthread.h>

#include "ripemd160_256.h"

#include "ec_pubkey_fast.h"

#include "hex.h"
#include "bloom.h"
#include "mmapf.h"
#include "hash160.h"
#include "hsearchf.h"

#include "algo/brainv2.h"
#include "algo/warpwallet.h"
#include "algo/brainwalletio.h"
#include "algo/sha3.h"

#define MAX_THREADS 16

/* 全局变量 */
static int api_notify_enabled = 0;
static char *api_endpoint = NULL;
static char *api_key = NULL;
static char *coin_type = "BTC"; // 默认币种
static pthread_mutex_t io_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t found_count = 0;

// raise this if you really want, but quickly diminishing returns
#define BATCH_MAX 4096

static int brainflayer_is_init = 0;

typedef struct pubhashfn_s {
   void (*fn)(hash160_t *, const unsigned char *);
   char id;
} pubhashfn_t;

static unsigned char *mem;

static mmapf_ctx bloom_mmapf;
static unsigned char *bloom = NULL;

static unsigned char *unhexed = NULL;
static size_t unhexed_sz = 4096;

#define bail(code, ...) \
do { \
  fprintf(stderr, __VA_ARGS__); \
  exit(code); \
} while (0)

#define chkmalloc(S) _chkmalloc(S, __FILE__, __LINE__)
static void * _chkmalloc(size_t size, unsigned char *file, unsigned int line) {
  void *ptr = malloc(size);
  if (ptr == NULL) {
    bail(1, "malloc(%zu) failed at %s:%u: %s\n", size, file, line, strerror(errno));
  }
  return ptr;
}

#define chkrealloc(P, S) _chkrealloc(P, S, __FILE__, __LINE__);
static void * _chkrealloc(void *ptr, size_t size, unsigned char *file, unsigned int line) {
  void *ptr2 = realloc(ptr, size);
  if (ptr2 == NULL) {
    bail(1, "realloc(%p, %zu) failed at %s:%u: %s\n", ptr, size, file, line, strerror(errno));
  }
  return ptr2;
}

static uint64_t getns() {
  uint64_t ns;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  ns  = ts.tv_nsec;
  ns += ts.tv_sec * 1000000000ULL;
  return ns;
}

/* 线程上下文结构 */
typedef struct {
    int thread_id;
    int batch_size;
    char **batch_lines;
    size_t *batch_line_sz;
    unsigned char **batch_priv;
    unsigned char **batch_upub;
    unsigned char *bloom;
    FILE *filter_file;
    FILE *output_file;
    pthread_mutex_t *io_mutex;
    uint64_t *found_count;
    pubhashfn_t *pubhashfn;
    unsigned char *modestr;
} thread_context_t;

/* 发送HTTP通知的函数 */
int send_collision_notification(const char *address, const char *private_key, 
                               const char *coin_type, unsigned char compressed) {
    CURL *curl;
    CURLcode res;
    struct curl_slist *headers = NULL;
    char post_data[1024];
    char *compressed_str = compressed == 'u' ? "uncompressed" : "compressed";
    
    // 如果API通知未启用，直接返回
    if (!api_notify_enabled || !api_endpoint || !api_key) {
        return 0;
    }
    
    // 初始化curl
    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "无法初始化CURL\n");
        return 1;
    }
    
    // 构建JSON数据
    snprintf(post_data, sizeof(post_data), 
             "{\"coin_type\":\"%s\",\"address\":\"%s\",\"private_key\":\"%s\",\"compressed\":\"%s\",\"timestamp\":%ld}",
             coin_type, address, private_key, compressed_str, (long)time(NULL));
    
    // 设置请求头
    headers = curl_slist_append(headers, "Content-Type: application/json");
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "X-API-Key: %s", api_key);
    headers = curl_slist_append(headers, auth_header);
    
    // 设置CURL选项
    curl_easy_setopt(curl, CURLOPT_URL, api_endpoint);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L); // 10秒超时
    
    // 执行请求
    res = curl_easy_perform(curl);
    
    // 检查结果
    if (res != CURLE_OK) {
        fprintf(stderr, "发送通知失败: %s\n", curl_easy_strerror(res));
    } else {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code >= 200 && http_code < 300) {
            fprintf(stderr, "\033[1;32m已发送碰撞通知到API\033[0m\n");
        } else {
            fprintf(stderr, "\033[1;31mAPI返回错误状态码: %ld\033[0m\n", http_code);
        }
    }
    
    // 清理
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    return (res == CURLE_OK) ? 0 : 1;
}

/* 线程安全的结果输出函数 */
void thread_safe_print_result(thread_context_t *ctx, hash160_t *hash, 
                             unsigned char compressed, unsigned char *type, 
                             unsigned char *input) {
    pthread_mutex_lock(ctx->io_mutex);
    
    unsigned char hexed0[41];
    char *address = hex(hash->uc, 20, hexed0, sizeof(hexed0));
    
    // 输出到文件
    fprintf(ctx->output_file, "%s:%c:%s:%s\n",
            address, compressed, type, input);
    
    // 增加计数
    (*ctx->found_count)++;
    
    // 发送API通知
    if (api_notify_enabled) {
        send_collision_notification(address, (char *)input, coin_type, compressed);
        
        // 在终端显示彩色提醒
        fprintf(stderr, "\033[1;31m发现匹配！地址: %s 私钥: %s\033[0m\n", 
                address, input);
    }
    
    pthread_mutex_unlock(ctx->io_mutex);
}

static inline void brainflayer_init_globals() {
  /* only initialize stuff once */
  if (!brainflayer_is_init) {
    /* initialize buffers */
    mem = chkmalloc(4096);
    unhexed = chkmalloc(unhexed_sz);

    /* set the flag */
    brainflayer_is_init = 1;
  }
}

// function pointers
static int (*input2priv)(unsigned char *, unsigned char *, size_t);

/* bitcoin uncompressed address */
static void uhash160(hash160_t *h, const unsigned char *upub) {
  SHA256_CTX ctx;
  unsigned char hash[SHA256_DIGEST_LENGTH];

  SHA256_Init(&ctx);
  SHA256_Update(&ctx, upub, 65);
  SHA256_Final(hash, &ctx);
  ripemd160_256(hash, h->uc);
}

/* bitcoin compressed address */
static void chash160(hash160_t *h, const unsigned char *upub) {
  SHA256_CTX ctx;
  unsigned char cpub[33];
  unsigned char hash[SHA256_DIGEST_LENGTH];

  /* quick and dirty public key compression */
  cpub[0] = 0x02 | (upub[64] & 0x01);
  memcpy(cpub + 1, upub + 1, 32);
  SHA256_Init(&ctx);
  SHA256_Update(&ctx, cpub, 33);
  SHA256_Final(hash, &ctx);
  ripemd160_256(hash, h->uc);
}

/* ethereum address */
static void ehash160(hash160_t *h, const unsigned char *upub) {
  SHA3_256_CTX ctx;
  unsigned char hash[SHA256_DIGEST_LENGTH];

  /* compute hash160 for uncompressed public key */
  /* keccak_256_last160(pub) */
  KECCAK_256_Init(&ctx);
  KECCAK_256_Update(&ctx, upub+1, 64);
  KECCAK_256_Final(hash, &ctx);
  memcpy(h->uc, hash+12, 20);
}

/* msb of x coordinate of public key */
static void xhash160(hash160_t *h, const unsigned char *upub) {
  memcpy(h->uc, upub+1, 20);
}



static int pass2priv(unsigned char *priv, unsigned char *pass, size_t pass_sz) {
  SHA256_CTX ctx;

  SHA256_Init(&ctx);
  SHA256_Update(&ctx, pass, pass_sz);
  SHA256_Final(priv, &ctx);

  return 0;
}

static int keccak2priv(unsigned char *priv, unsigned char *pass, size_t pass_sz) {
  SHA3_256_CTX ctx;

  KECCAK_256_Init(&ctx);
  KECCAK_256_Update(&ctx, pass, pass_sz);
  KECCAK_256_Final(priv, &ctx);

  return 0;
}

/* ether.camp "2031 passes of SHA-3 (Keccak)" */
static int camp2priv(unsigned char *priv, unsigned char *pass, size_t pass_sz) {
  SHA3_256_CTX ctx;
  int i;

  KECCAK_256_Init(&ctx);
  KECCAK_256_Update(&ctx, pass, pass_sz);
  KECCAK_256_Final(priv, &ctx);

  for (i = 1; i < 2031; ++i) {
    KECCAK_256_Init(&ctx);
    KECCAK_256_Update(&ctx, priv, 32);
    KECCAK_256_Final(priv, &ctx);
  }

  return 0;
}

static int sha32priv(unsigned char *priv, unsigned char *pass, size_t pass_sz) {
  SHA3_256_CTX ctx;

  SHA3_256_Init(&ctx);
  SHA3_256_Update(&ctx, pass, pass_sz);
  SHA3_256_Final(priv, &ctx);

  return 0;
}

/*
static int dicap2hash160(unsigned char *pass, size_t pass_sz) {
  SHA3_256_CTX ctx;

  int i, ret;

  KECCAK_256_Init(&ctx);
  KECCAK_256_Update(&ctx, pass, pass_sz);
  KECCAK_256_Final(priv256, &ctx);
  for (i = 0; i < 16384; ++i) {
    KECCAK_256_Init(&ctx);
    KECCAK_256_Update(&ctx, priv256, 32);
    KECCAK_256_Final(priv256, &ctx);
  }

  for (;;) {
    ret = priv2hash160(priv256);
    if (hash160_uncmp.uc[0] == 0) { break; }
    KECCAK_256_Init(&ctx);
    KECCAK_256_Update(&ctx, priv256, 32);
    KECCAK_256_Final(priv256, &ctx);
  }
  return ret;
}
*/

static int rawpriv2priv(unsigned char *priv, unsigned char *rawpriv, size_t rawpriv_sz) {
  memcpy(priv, rawpriv, rawpriv_sz);
  return 0;
}

static unsigned char *kdfsalt;
static size_t kdfsalt_sz;

static int warppass2priv(unsigned char *priv, unsigned char *pass, size_t pass_sz) {
  int ret;
  if ((ret = warpwallet(pass, pass_sz, kdfsalt, kdfsalt_sz, priv)) != 0) return ret;
  pass[pass_sz] = 0;
  return 0;
}

static int bwiopass2priv(unsigned char *priv, unsigned char *pass, size_t pass_sz) {
  int ret;
  if ((ret = brainwalletio(pass, pass_sz, kdfsalt, kdfsalt_sz, priv)) != 0) return ret;
  pass[pass_sz] = 0;
  return 0;
}

static int brainv2pass2priv(unsigned char *priv, unsigned char *pass, size_t pass_sz) {
  unsigned char hexout[33];
  int ret;
  if ((ret = brainv2(pass, pass_sz, kdfsalt, kdfsalt_sz, hexout)) != 0) return ret;
  pass[pass_sz] = 0;
  return pass2priv(priv, hexout, sizeof(hexout)-1);
}

static unsigned char *kdfpass;
static size_t kdfpass_sz;

static int warpsalt2priv(unsigned char *priv, unsigned char *salt, size_t salt_sz) {
  int ret;
  if ((ret = warpwallet(kdfpass, kdfpass_sz, salt, salt_sz, priv)) != 0) return ret;
  salt[salt_sz] = 0;
  return 0;
}

static int bwiosalt2priv(unsigned char *priv, unsigned char *salt, size_t salt_sz) {
  int ret;
  if ((ret = brainwalletio(kdfpass, kdfpass_sz, salt, salt_sz, priv)) != 0) return ret;
  salt[salt_sz] = 0;
  return 0;
}

static int brainv2salt2priv(unsigned char *priv, unsigned char *salt, size_t salt_sz) {
  unsigned char hexout[33];
  int ret;
  if ((ret = brainv2(kdfpass, kdfpass_sz, salt, salt_sz, hexout)) != 0) return ret;
  salt[salt_sz] = 0;
  return pass2priv(priv, hexout, sizeof(hexout)-1);
}

static unsigned char rushchk[5];
static int rush2priv(unsigned char *priv, unsigned char *pass, size_t pass_sz) {
  SHA256_CTX ctx;
  unsigned char hash[SHA256_DIGEST_LENGTH];
  unsigned char userpasshash[SHA256_DIGEST_LENGTH*2+1];

  SHA256_Init(&ctx);
  SHA256_Update(&ctx, pass, pass_sz);
  SHA256_Final(hash, &ctx);

  hex(hash, sizeof(hash), userpasshash, sizeof(userpasshash));

  SHA256_Init(&ctx);
  // kdfsalt should be the fragment up to the !
  SHA256_Update(&ctx, kdfsalt, kdfsalt_sz);
  SHA256_Update(&ctx, userpasshash, 64);
  SHA256_Final(priv, &ctx);

  // early exit if the checksum doesn't match
  if (memcmp(priv, rushchk, sizeof(rushchk)) != 0) { return -1; }

  return 0;
}

inline static int priv_incr(unsigned char *upub, unsigned char *priv) {
  int sz;

  secp256k1_ec_pubkey_incr(upub, &sz, priv);

  return 0;
}

inline static void priv2pub(unsigned char *upub, const unsigned char *priv) {
  int sz;

  secp256k1_ec_pubkey_create_precomp(upub, &sz, priv);
}


inline static void fprintresult(FILE *f, hash160_t *hash,
                                unsigned char compressed,
                                unsigned char *type,
                                unsigned char *input) {
  unsigned char hexed0[41];
  char *address = hex(hash->uc, 20, hexed0, sizeof(hexed0));

  // 原始输出
  fprintf(f, "%s:%c:%s:%s\n",
          address,
          compressed,
          type,
          input);
  
  // 发送API通知
  if (api_notify_enabled) {
      send_collision_notification(address, (char *)input, coin_type, compressed);
      
      // 在终端显示彩色提醒
      fprintf(stderr, "\033[1;31m发现匹配！地址: %s 私钥: %s\033[0m\n", 
              address, input);
  }
}

void usage(unsigned char *name) {
  printf("Usage: %s [OPTION]...\n\n\
 -a                          open output file in append mode\n\
 -b FILE                     check for matches against bloom filter FILE\n\
 -f FILE                     verify matches against sorted hash160s in FILE\n\
 -i FILE                     read from FILE instead of stdin\n\
 -o FILE                     write to FILE instead of stdout\n\
 -c TYPES                    use TYPES for public key to hash160 computation\n\
                             multiple can be specified, for example the default\n\
                             is 'uc', which will check for both uncompressed\n\
                             and compressed addresses using Bitcoin's algorithm\n\
                             u - uncompressed address\n\
                             c - compressed address\n\
                             e - ethereum address\n\
                             x - most signifigant bits of x coordinate\n\
 -t TYPE                     inputs are TYPE - supported types:\n\
                             sha256 (default) - classic brainwallet\n\
                             sha3   - sha3-256\n\
                             priv   - raw private keys (requires -x)\n\
                             warp   - WarpWallet (supports -s or -p)\n\
                             bwio   - brainwallet.io (supports -s or -p)\n\
                             bv2    - brainv2 (supports -s or -p) VERY SLOW\n\
                             rush   - rushwallet (requires -r) FAST\n\
                             keccak - keccak256 (ethercamp/old ethaddress)\n\
                             camp2  - keccak256 * 2031 (new ethercamp)\n\
 -x                          treat input as hex encoded\n\
 -s SALT                     use SALT for salted input types (default: none)\n\
 -p PASSPHRASE               use PASSPHRASE for salted input types, inputs\n\
                             will be treated as salts\n\
 -r RUSHWALLET               rushwallet.com password, all other options become\n\
                             the fragment (the part after #)\n\
 -I PRIVKEY                  incremental private key (hex) (default: random)\n\
 -k K                        skip the first K private keys\n\
 -n K/N                      use only the Kth of every N private keys\n\
 -B BATCH_SIZE               number of keys per batch (default: 1)\n\
 -m FILE                     load ecmult table from FILE\n\
 -v                          verbose - display cracking progress\n\
 -A URL                      启用API通知并设置端点URL\n\
 -K KEY                      设置API密钥\n\
 -C TYPE                     设置币种类型 (默认: BTC)\n\
 -T NUM                      设置线程数 (默认: 自动检测)\n\
 -h                          show this help\n", name);
}

int main(int argc, char **argv) {
  FILE *ifile = stdin;
  FILE *ofile = stdout;
  FILE *ffile = NULL;

  int ret, c, i, j;

  float alpha, ilines_rate, ilines_rate_avg;
  int64_t raw_lines = -1;
  uint64_t report_mask = 0;
  uint64_t time_last, time_curr, time_delta;
  uint64_t time_start, time_elapsed;
  uint64_t ilines_last, ilines_curr, ilines_delta;
  uint64_t olines;

  int skipping = 0, tty = 0;

  unsigned char modestr[64];

  int spok = 0, aopt = 0, vopt = 0, wopt = 16, xopt = 0;
  int nopt_mod = 0, nopt_rem = 0, Bopt = 0;
  uint64_t kopt = 0, Nopt = ~0ULL;
  unsigned char *bopt = NULL, *iopt = NULL, *oopt = NULL;
  unsigned char *topt = NULL, *sopt = NULL, *popt = NULL;
  unsigned char *mopt = NULL, *fopt = NULL, *ropt = NULL;
  unsigned char *Iopt = NULL, *copt = NULL;
  
  // 新增参数
  int num_threads = 0; // 0表示自动检测
  
  // 添加缺失的变量声明
  unsigned char priv[64];
  hash160_t hash160;
  pubhashfn_t pubhashfn[8];
  memset(pubhashfn, 0, sizeof(pubhashfn));
  
  int batch_stopped = -1;
  char *batch_line[BATCH_MAX];
  size_t batch_line_sz[BATCH_MAX];
  int batch_line_read[BATCH_MAX];
  unsigned char batch_priv[BATCH_MAX][32];
  unsigned char batch_upub[BATCH_MAX][65];

  // 初始化CURL
  curl_global_init(CURL_GLOBAL_DEFAULT);

  while ((c = getopt(argc, argv, "ab:c:f:hi:k:m:n:o:p:r:s:t:vw:xB:I:N:A:K:C:T:")) != -1) {
    switch (c) {
      case 'a':
        aopt = 1; // open output file in append mode
        break;
      case 'k':
        kopt = strtoull(optarg, NULL, 10); // skip first k lines of input
        skipping = 1;
        break;
      case 'n':
        // only try the rem'th of every mod lines (one indexed)
        nopt_rem = atoi(optarg) - 1;
        optarg = strchr(optarg, '/');
        if (optarg != NULL) { nopt_mod = atoi(optarg+1); }
        skipping = 1;
        break;
      case 'B':
        Bopt = atoi(optarg);
        break;
      case 'N':
        Nopt = strtoull(optarg, NULL, 0); // allows 0x
        break;
      case 'w':
        if (wopt > 1) wopt = atoi(optarg);
        break;
      case 'm':
        mopt = optarg; // table file
        wopt = 1; // auto
        break;
      case 'v':
        vopt = 1; // verbose
        break;
      case 'b':
        bopt = optarg; // bloom filter file
        break;
      case 'f':
        fopt = optarg; // full filter file
        break;
      case 'i':
        iopt = optarg; // input file
        break;
      case 'o':
        oopt = optarg; // output file
        break;
      case 'x':
        xopt = 1; // input is hex encoded
        break;
      case 's':
        sopt = optarg; // salt
        break;
      case 'p':
        popt = optarg; // passphrase
        break;
      case 'r':
        ropt = optarg; // rushwallet
        break;
      case 'c':
        copt = optarg; // type of hash160
        break;
      case 't':
        topt = optarg; // type of input
        break;
      case 'I':
        Iopt = optarg; // start key for incremental
        xopt = 1; // input is hex encoded
        break;
      case 'h':
        // show help
        usage(argv[0]);
        return 0;
      case 'A':
        api_notify_enabled = 1;
        api_endpoint = optarg;
        break;
      case 'K':
        api_key = optarg;
        break;
      case 'C':
        coin_type = optarg;
        break;
      case 'T':
        num_threads = atoi(optarg);
        if (num_threads < 1) num_threads = 1;
        if (num_threads > MAX_THREADS) num_threads = MAX_THREADS;
        break;
      case '?':
        // show error
        return 1;
      default:
        // should never be reached...
        printf("got option '%c' (%d)\n", c, c);
        return 1;
    }
  }

  if (optind < argc) {
    if (optind == 1 && argc == 2) {
      // older versions of brainflayer had the bloom filter file as a
      // single optional argument, this keeps compatibility with that
      bopt = argv[1];
    } else {
      fprintf(stderr, "Invalid arguments:\n");
      while (optind < argc) {
        fprintf(stderr, "    '%s'\n", argv[optind++]);
      }
      exit(1);
    }
  }

  if (nopt_rem != 0 || nopt_mod != 0) {
    // note that nopt_rem has had one subtracted at option parsing
    if (nopt_rem >= nopt_mod) {
      bail(1, "Invalid '-n' argument, remainder '%d' must be <= modulus '%d'\n", nopt_rem+1, nopt_mod);
    } else if (nopt_rem < 0) {
      bail(1, "Invalid '-n' argument, remainder '%d' must be > 0\n", nopt_rem+1);
    } else if (nopt_mod < 1) {
      bail(1, "Invalid '-n' argument, modulus '%d' must be > 0\n", nopt_mod);
    }
  }

  if (wopt < 1 || wopt > 28) {
    bail(1, "Invalid window size '%d' - must be >= 1 and <= 28\n", wopt);
  } else {
    // very rough sanity check of window size
    struct sysinfo info;
    sysinfo(&info);
    uint64_t sysram = info.mem_unit * info.totalram;
    if (3584LLU*(1<<wopt) > sysram) {
      bail(1, "Not enough ram for requested window size '%d'\n", wopt);
    }
  }

  if (Bopt) { // if unset, will be set later
    if (Bopt < 1 || Bopt > BATCH_MAX) {
      bail(1, "Invalid '-B' argument, batch size '%d' - must be >= 1 and <= %d\n", Bopt, BATCH_MAX);
    } else if (Bopt & (Bopt - 1)) { // https://graphics.stanford.edu/~seander/bithacks.html#DetermineIfPowerOf2
      bail(1, "Invalid '-B' argument, batch size '%d' is not a power of 2\n", Bopt);
    }
  }

  if (Iopt) {
    if (strlen(Iopt) != 64) {
      bail(1, "The starting key passed to the '-I' must be 64 hex digits exactly\n");
    }
    if (topt) {
      bail(1, "Cannot specify input type in incremental mode\n");
    }
    topt = "priv";
    // normally, getline would allocate the batch_line entries, but we need to
    // do this to give the processing loop somewhere to write to in incr mode
    for (i = 0; i < BATCH_MAX; ++i) {
      batch_line[i] = Iopt;
    }
    unhex(Iopt, sizeof(priv)*2, priv, sizeof(priv));
    skipping = 1;
    if (!nopt_mod) { nopt_mod = 1; };
  }


  /* handle copt */
  if (copt == NULL) { copt = "uc"; }
  i = 0;
  while (copt[i]) {
    switch (copt[i]) {
      case 'u':
        pubhashfn[i].fn = &uhash160;
        break;
      case 'c':
        pubhashfn[i].fn = &chash160;
        break;
      case 'e':
        pubhashfn[i].fn = &ehash160;
        break;
      case 'x':
        pubhashfn[i].fn = &xhash160;
        break;
      default:
        bail(1, "Unknown hash160 type '%c'.\n", copt[i]);
    }
    if (strchr(copt + i + 1, copt[i])) {
      bail(1, "Duplicate hash160 type '%c'.\n", copt[i]);
    }
    pubhashfn[i].id = copt[i];
    ++i;
  }

  /* handle topt */
  if (topt == NULL) { topt = "sha256"; }

  if (strcmp(topt, "sha256") == 0) {
    input2priv = &pass2priv;
  } else if (strcmp(topt, "priv") == 0) {
    if (!xopt) {
      bail(1, "raw private key input requires -x");
    }
    input2priv = &rawpriv2priv;
  } else if (strcmp(topt, "warp") == 0) {
    if (!Bopt) { Bopt = 1; } // don't batch transform for slow input hashes by default
    spok = 1;
    input2priv = popt ? &warpsalt2priv : &warppass2priv;
  } else if (strcmp(topt, "bwio") == 0) {
    if (!Bopt) { Bopt = 1; } // don't batch transform for slow input hashes by default
    spok = 1;
    input2priv = popt ? &bwiosalt2priv : &bwiopass2priv;
  } else if (strcmp(topt, "bv2") == 0) {
    if (!Bopt) { Bopt = 1; } // don't batch transform for slow input hashes by default
    spok = 1;
    input2priv = popt ? &brainv2salt2priv : &brainv2pass2priv;
  } else if (strcmp(topt, "rush") == 0) {
    input2priv = &rush2priv;
  } else if (strcmp(topt, "camp2") == 0) {
    input2priv = &camp2priv;
  } else if (strcmp(topt, "keccak") == 0) {
    input2priv = &keccak2priv;
  } else if (strcmp(topt, "sha3") == 0) {
    input2priv = &sha32priv;
//  } else if (strcmp(topt, "dicap") == 0) {
//    input2priv = &dicap2priv;
  } else {
    bail(1, "Unknown input type '%s'.\n", topt);
  }

  if (spok) {
    if (sopt && popt) {
      bail(1, "Cannot specify both a salt and a passphrase\n");
    }
    if (popt) {
      kdfpass = popt;
      kdfpass_sz = strlen(popt);
    } else {
      if (sopt) {
        kdfsalt = sopt;
        kdfsalt_sz = strlen(kdfsalt);
      } else {
        kdfsalt = chkmalloc(0);
        kdfsalt_sz = 0;
      }
    }
  } else {
    if (popt) {
      bail(1, "Specifying a passphrase not supported with input type '%s'\n", topt);
    } else if (sopt) {
      bail(1, "Specifying a salt not supported with this input type '%s'\n", topt);
    }
  }

  if (ropt) {
    if (input2priv != &rush2priv) {
      bail(1, "Specifying a url fragment only supported with input type 'rush'\n");
    }
    kdfsalt = ropt;
    kdfsalt_sz = strlen(kdfsalt) - sizeof(rushchk)*2;
    if (kdfsalt[kdfsalt_sz-1] != '!') {
      bail(1, "Invalid rushwallet url fragment '%s'\n", kdfsalt);
    }
    unhex(kdfsalt+kdfsalt_sz, sizeof(rushchk)*2, rushchk, sizeof(rushchk));
    kdfsalt[kdfsalt_sz] = '\0';
  } else if (input2priv == &rush2priv) {
    bail(1, "The '-r' option is required for rushwallet.\n");
  }

  snprintf(modestr, sizeof(modestr), xopt ? "(hex)%s" : "%s", topt);

  if (bopt) {
    if ((ret = mmapf(&bloom_mmapf, bopt, BLOOM_SIZE, MMAPF_RNDRD)) != MMAPF_OKAY) {
      bail(1, "failed to open bloom filter '%s': %s\n", bopt, mmapf_strerror(ret));
    } else if (bloom_mmapf.mem == NULL) {
      bail(1, "got NULL pointer trying to set up bloom filter\n");
    }
    bloom = bloom_mmapf.mem;
  }

  if (fopt) {
    if (!bopt) {
      bail(1, "The '-f' option must be used with a bloom filter\n");
    }
    if ((ffile = fopen(fopt, "r")) == NULL) {
      bail(1, "failed to open '%s' for reading: %s\n", fopt, strerror(errno));
    }
  }

  if (iopt) {
    if ((ifile = fopen(iopt, "r")) == NULL) {
      bail(1, "failed to open '%s' for reading: %s\n", iopt, strerror(errno));
    }
    // increases readahead window, don't really care if it fails
    posix_fadvise(fileno(ifile), 0, 0, POSIX_FADV_SEQUENTIAL);
  }

  if (oopt && (ofile = fopen(oopt, (aopt ? "a" : "w"))) == NULL) {
    bail(1, "failed to open '%s' for writing: %s\n", oopt, strerror(errno));
  }

  /* line buffer output */
  setvbuf(ofile,  NULL, _IOLBF, 0);
  /* line buffer stderr */
  setvbuf(stderr, NULL, _IOLBF, 0);

  if (vopt && ofile == stdout && isatty(fileno(stdout))) { tty = 1; }

  brainflayer_init_globals();

  if (secp256k1_ec_pubkey_precomp_table(wopt, mopt) != 0) {
    bail(1, "failed to initialize precomputed table\n");
  }

  if (secp256k1_ec_pubkey_batch_init(BATCH_MAX) != 0) {
    bail(1, "failed to initialize batch point conversion structures\n");
  }

  ilines_curr = 0;

  if (vopt) {
    /* initialize timing data */
    time_start = time_last = getns();
    olines = ilines_last = 0;
    ilines_rate_avg = -1;
    alpha = 0.500;
  } else {
    time_start = time_last = 0; // prevent compiler warning about uninitialized use
  }

  // set default batch size
  if (!Bopt) { Bopt = BATCH_MAX; }

  for (;;) {
    if (Iopt) {
      if (skipping) {
        priv_add_uint32(priv, nopt_rem + kopt);
        skipping = 0;
      }
      secp256k1_ec_pubkey_batch_incr(Bopt, nopt_mod, batch_upub, batch_priv, priv);
      memcpy(priv, batch_priv[Bopt-1], 32);
      priv_add_uint32(priv, nopt_mod);

      batch_stopped = Bopt;
    } else {
      for (i = 0; i < Bopt; ++i) {
        if ((batch_line_read[i] = getline(&batch_line[i], &batch_line_sz[i], ifile)-1) > -1) {
          if (skipping) {
            ++raw_lines;
            if (kopt && raw_lines < kopt) { --i; continue; }
            if (nopt_mod && raw_lines % nopt_mod != nopt_rem) { --i; continue; }
          }
        } else {
          break;
        }
        batch_line[i][batch_line_read[i]] = 0;
        if (xopt) {
          if (batch_line_read[i] / 2 > unhexed_sz) {
            unhexed_sz = batch_line_read[i];
            unhexed = chkrealloc(unhexed, unhexed_sz);
          }
          // rewrite the input line from hex
          unhex(batch_line[i], batch_line_read[i], unhexed, unhexed_sz);
          if (input2priv(batch_priv[i], unhexed, batch_line_read[i]/2) != 0) {
            fprintf(stderr, "input2priv failed! continuing...\n");
          }
        } else {
          if (input2priv(batch_priv[i], batch_line[i], batch_line_read[i]) != 0) {
            fprintf(stderr, "input2priv failed! continuing...\n");
          }
        }
      }

      // batch compute the public keys
      secp256k1_ec_pubkey_batch_create(Bopt, batch_upub, batch_priv);

      // save ending value from read loop
      batch_stopped = i;
    }

    // loop over the public keys
    for (i = 0; i < batch_stopped; ++i) {
      if (bloom) { /* crack mode */
        // loop over pubkey hash functions
        for (j = 0; pubhashfn[j].fn != NULL; ++j) {
          pubhashfn[j].fn(&hash160, batch_upub[i]);

          unsigned int bit;
          bit = BH00(hash160.ul); if (BLOOM_GET_BIT(bit) == 0) { continue; }
          bit = BH01(hash160.ul); if (BLOOM_GET_BIT(bit) == 0) { continue; }
          bit = BH02(hash160.ul); if (BLOOM_GET_BIT(bit) == 0) { continue; }
          bit = BH03(hash160.ul); if (BLOOM_GET_BIT(bit) == 0) { continue; }
          bit = BH04(hash160.ul); if (BLOOM_GET_BIT(bit) == 0) { continue; }
          bit = BH05(hash160.ul); if (BLOOM_GET_BIT(bit) == 0) { continue; }
          bit = BH06(hash160.ul); if (BLOOM_GET_BIT(bit) == 0) { continue; }
          bit = BH07(hash160.ul); if (BLOOM_GET_BIT(bit) == 0) { continue; }
          bit = BH08(hash160.ul); if (BLOOM_GET_BIT(bit) == 0) { continue; }
          bit = BH09(hash160.ul); if (BLOOM_GET_BIT(bit) == 0) { continue; }
          bit = BH10(hash160.ul); if (BLOOM_GET_BIT(bit) == 0) { continue; }
          bit = BH11(hash160.ul); if (BLOOM_GET_BIT(bit) == 0) { continue; }
          bit = BH12(hash160.ul); if (BLOOM_GET_BIT(bit) == 0) { continue; }
          bit = BH13(hash160.ul); if (BLOOM_GET_BIT(bit) == 0) { continue; }
          bit = BH14(hash160.ul); if (BLOOM_GET_BIT(bit) == 0) { continue; }
          bit = BH15(hash160.ul); if (BLOOM_GET_BIT(bit) == 0) { continue; }
          bit = BH16(hash160.ul); if (BLOOM_GET_BIT(bit) == 0) { continue; }
          bit = BH17(hash160.ul); if (BLOOM_GET_BIT(bit) == 0) { continue; }
          bit = BH18(hash160.ul); if (BLOOM_GET_BIT(bit) == 0) { continue; }
          bit = BH19(hash160.ul); if (BLOOM_GET_BIT(bit) == 0) { continue; }

          if (!fopt || hsearchf(ffile, &hash160)) {
            if (tty) { fprintf(ofile, "\033[0K"); }
            // reformat/populate the line if required
            if (Iopt) {
              hex(batch_priv[i], 32, batch_line[i], 65);
            }
            fprintresult(ofile, &hash160, pubhashfn[j].id, modestr, batch_line[i]);
            ++olines;
          }
        }
      } else { /* generate mode */
        // reformat/populate the line if required
        if (Iopt) {
          hex(batch_priv[i], 32, batch_line[i], 65);
        }
        j = 0;
        while (pubhashfn[j].fn != NULL) {
          pubhashfn[j].fn(&hash160, batch_upub[i]);
          fprintresult(ofile, &hash160, pubhashfn[j].id, modestr, batch_line[i]);
          ++j;
        }
      }
    }
    // end public key processing loop

    ilines_curr += batch_stopped;

    // start stats
    if (vopt) {
      ilines_curr += batch_stopped;
      if (batch_stopped < Bopt || (ilines_curr & report_mask) == 0) {
        time_curr = getns();
        time_delta = time_curr - time_last;
        time_elapsed = time_curr - time_start;
        time_last = time_curr;
        ilines_delta = ilines_curr - ilines_last;
        ilines_last = ilines_curr;
        ilines_rate = (ilines_delta * 1.0e9) / (time_delta * 1.0);

        if (batch_stopped < Bopt) {
          /* report overall average on last status update */
          ilines_rate_avg = (ilines_curr * 1.0e9) / (time_elapsed * 1.0);
        } else if (ilines_rate_avg < 0) {
          ilines_rate_avg = ilines_rate;
        /* target reporting frequency to about once every five seconds */
        } else if (time_delta < 2500000000) {
          report_mask = (report_mask << 1) | 1;
          ilines_rate_avg = ilines_rate; /* reset EMA */
        } else if (time_delta > 10000000000) {
          report_mask >>= 1;
          ilines_rate_avg = ilines_rate; /* reset EMA */
        } else {
          /* exponetial moving average */
          ilines_rate_avg = alpha * ilines_rate + (1 - alpha) * ilines_rate_avg;
        }

        fprintf(stderr,
            "\033[0G\033[2K"
            " rate: %9.2f p/s"
            " found: %5zu/%-10zu"
            " elapsed: %8.3f s"
            "\033[0G",
            ilines_rate_avg,
            olines,
            ilines_curr,
            time_elapsed / 1.0e9
        );

        fflush(stderr);
      }
    }
    // end stats

    // main loop exit condition
    if (batch_stopped < Bopt || ilines_curr >= Nopt) {
      if (vopt) { fprintf(stderr, "\n"); }
      break;
    }
  }

  // 在main函数结束前清理CURL
  curl_global_cleanup();

  return 0;
}

/*  vim: set ts=2 sw=2 et ai si: */
