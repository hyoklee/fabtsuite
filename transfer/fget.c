#include <assert.h>
#include <err.h>
#include <libgen.h> /* basename(3) */
#include <limits.h> /* INT_MAX */
#include <inttypes.h>   /* PRIu32 */
#include <signal.h>
#include <stdarg.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h> /* strcmp(3), strdup(3) */
#include <unistd.h> /* getopt(3), sysconf(3) */

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>     /* fi_listen, fi_getname */
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_rma.h>    /* struct fi_msg_rma */

#include "hlog.h"

#define arraycount(a)   (sizeof(a) / sizeof(a[0]))

/*
 * Message definitions
 */
typedef struct {
    uint64_t bits[2];
} nonce_t;

typedef struct initial_msg {
    nonce_t nonce;
    uint32_t nsources;
    uint32_t id;
    uint32_t addrlen;
    char addr[512];
} initial_msg_t;

typedef struct ack_msg {
    uint32_t addrlen;
    char addr[512];
} ack_msg_t;

typedef struct vector_msg {
    uint32_t niovs;
    struct {
        uint64_t addr, len, key;
    } iov[12];
} vector_msg_t;

typedef struct progress_msg {
    uint64_t nfilled;
    uint64_t nleftover;
} progress_msg_t;

/* Communication buffers */

typedef enum {
  xft_progress
, xft_rdma_write
, xft_vector
, xft_fragment
} xfc_type_t;

typedef enum {
    xfp_first = 0x1
,   xfp_last = 0x2
} xfc_place_t;

typedef enum {
    xfo_program = 0
,   xfo_nic = 1
} xfc_owner_t;

typedef struct {
    struct fi_context ctx;  // this has to be the first member
    uint32_t type:4;
    uint32_t owner:1;
    uint32_t place:2;
    uint32_t nchildren:8;
    uint32_t cancelled:1;
    uint32_t unused:16;
} xfer_context_t;

typedef struct completion {
    uint64_t flags;
    size_t len;
    xfer_context_t *xfc;
} completion_t;

typedef struct bufhdr {
    uint64_t raddr;
    size_t nused;
    size_t nallocated;
    struct fid_mr *mr;
    void *desc;
    xfer_context_t xfc;
} bufhdr_t;

typedef struct fragment {
    bufhdr_t hdr;
    bufhdr_t alignas(max_align_t) * parent;
} fragment_t;

typedef struct bytebuf {
    bufhdr_t hdr;
    char alignas(max_align_t) payload[];
} bytebuf_t;

typedef struct progbuf {
    bufhdr_t hdr;
    progress_msg_t alignas(max_align_t) msg;
} progbuf_t;

typedef struct vecbuf {
    bufhdr_t hdr;
    vector_msg_t alignas(max_align_t) msg;
} vecbuf_t;

typedef struct fifo {
    uint64_t insertions;
    uint64_t removals;
    size_t index_mask;  // for some integer n > 0, 2^n - 1 == index_mask
    bufhdr_t *hdr[];
} fifo_t;

typedef struct buflist {
    uint64_t access;
    size_t nfull;
    size_t nallocated;
    bufhdr_t *buf[];
} buflist_t;

/* Communication terminals: sources and sinks */

typedef enum {
    loop_continue
,   loop_end
,   loop_error
} loop_control_t;

struct terminal;
typedef struct terminal terminal_t;

struct terminal {
    /* trade(t, ready, completed) */
    loop_control_t (*trade)(terminal_t *, fifo_t *, fifo_t *);
    bool eof;
};

typedef struct {
    terminal_t terminal;
    size_t idx;
    size_t txbuflen;
    size_t entirelen;
} sink_t;

typedef struct {
    terminal_t terminal;
    size_t idx;
    size_t txbuflen;
    size_t entirelen;
} source_t;

typedef struct keysource {
    uint64_t next_key;
} keysource_t;

/*
 * Communications state definitions
 */

struct cxn;
typedef struct cxn cxn_t;

struct session;
typedef struct session session_t;

struct worker;
typedef struct worker worker_t;

struct cxn {
    loop_control_t (*loop)(worker_t *, session_t *);
    struct fid_ep *ep;
    struct fid_eq *eq;
    fi_addr_t peer_addr;
    struct fid_cq *cq;
    struct fid_av *av;
    bool cancelled;
    bool started;
    /* TBD break this condition into remote_eof, local_eof: receiver
     * needs to send an empty vector.msg.niovs == 0 to close, sender
     * needs to send progress.msg.nleftover == 0, record having received the
     * remote close in remote_eof and having completed sending it in
     * local_eof.
     */
    struct {
        bool local, remote;
    } eof;
    keysource_t keys;
};

typedef struct {
    fifo_t *posted; // buffers posted for vector messages
    fifo_t *rcvd;   // buffers holding received vector messages
} rxctl_t;

typedef struct {
    fifo_t *ready;    // message buffers ready to transmit
    fifo_t *posted;     // buffers posted with messages
    buflist_t *pool;    // unused buffers
} txctl_t;

typedef struct {
    cxn_t cxn;
    uint64_t nfull;
    fifo_t *tgtposted; // posted RDMA target buffers in order of issuance
    struct {
        struct iovec iov[12];
        void *desc[12];
        struct fid_mr *mr[12];
        uint64_t raddr[12];
        ssize_t niovs;
        ack_msg_t msg;
    } ack;
    struct {
        struct iovec iov[12];
        void *desc[12];
        struct fid_mr *mr[12];
        uint64_t raddr[12];
        ssize_t niovs;
        initial_msg_t msg;
    } initial;
    txctl_t vec;
    rxctl_t progress;
} rcvr_t;

typedef struct {
    cxn_t cxn;
    fifo_t *wrposted;  // posted RDMA writes in order of issuance
    size_t bytes_progress;
    rxctl_t vec;
    txctl_t progress;
    struct {
        void *desc;
        struct fid_mr *mr;
        initial_msg_t msg;
    } initial;
    struct {
        void *desc;
        struct fid_mr *mr;
        ack_msg_t msg;
    } ack;
    struct {
        struct iovec iov[12];
        void *desc[12];
        struct iovec iov2[12];
        void *desc2[12];
        struct fid_mr *mr[12];
        uint64_t raddr[12];
        ssize_t niovs;
        struct fi_context context;
    } payload;
    struct {
        buflist_t *pool;    // unused fragment headers
        size_t offset;      // offset into buffer at head of ready_for_cxn
    } fragment;
    struct fi_rma_iov riov[12], riov2[12];
    size_t nriovs;
    size_t next_riov;
    bool phase;
} xmtr_t;

/* On each loop, a worker checks its poll set for any completions.
 * If `loops_since_mark < UINT16_MAX`, a worker increases it by
 * one, and increases `ctxs_serviced_since_mark` by the number
 * of queues that actually held one or more completions.  If
 * `loops_since_mark == UINT16_MAX`, then a worker updates
 * `average`, `average = (average + 256 * ctxs_serviced_since_mark /
 * (UINT16_MAX + 1)) / 2`, and resets `loops_since_mark` and
 * `ctxs_serviced_since_mark` to 0.
 */
typedef struct {
    /* A fixed-point number with 8 bits right of the decimal point: */
    volatile atomic_uint_fast16_t average;
    uint_fast16_t loops_since_mark;
    uint32_t ctxs_serviced_since_mark;
} loadavg_t;

#define WORKER_SESSIONS_MAX 64
#define WORKERS_MAX 128
#define SESSIONS_MAX (WORKER_SESSIONS_MAX * WORKERS_MAX)

struct session {
    terminal_t *terminal;
    cxn_t *cxn;
    fifo_t *ready_for_cxn;
    fifo_t *ready_for_terminal;
};

struct worker {
    pthread_t thd;
    loadavg_t avg;
    terminal_t *term[WORKER_SESSIONS_MAX];
    session_t session[WORKER_SESSIONS_MAX];
    volatile _Atomic size_t nsessions[2];   // number of sessions in each half
                                            // of session[]
    struct fid_poll *pollset[2];
    pthread_mutex_t mtx[2]; /* mtx[0] protects pollset[0] and the first half
                             * of session[]; mtx[1], pollset[1] and the second
                             * half
                             */
    pthread_cond_t sleep;   /* Used in conjunction with workers_mtx. */
    volatile atomic_bool cancelled;
    bool failed;
    struct {
        buflist_t *tx;
        buflist_t *rx;
    } paybufs;    /* Reservoirs for free payload buffers. */
    keysource_t keys;
};

typedef struct {
    struct fid_eq *listen_eq;
    struct fid_ep *listen_ep;
    struct fid_cq *listen_cq;
    sink_t sink;
    rcvr_t rcvr;
} get_state_t;

typedef struct {
    xmtr_t xmtr;
    source_t source;
} put_state_t;

typedef struct {
    struct fid_domain *domain;
    struct fid_fabric *fabric;
    struct fi_info *info;
    union {
        get_state_t get;
        put_state_t put;
    } u;
    size_t mr_maxsegs;
    size_t rx_maxsegs;
    size_t tx_maxsegs;
    size_t rma_maxsegs;
    keysource_t keys;
    bool contiguous;
    bool reregister;
} state_t;

typedef int (*personality_t)(void);

HLOG_OUTLET_MEDIUM_DEFN(err, all, HLOG_OUTLET_S_ON);
HLOG_OUTLET_SHORT_DEFN(close, all);
HLOG_OUTLET_SHORT_DEFN(signal, all);
HLOG_OUTLET_SHORT_DEFN(params, all);
HLOG_OUTLET_SHORT_DEFN(tx_start, all);
HLOG_OUTLET_SHORT_DEFN(cxn_loop, all);
HLOG_OUTLET_SHORT_DEFN(write, all);
HLOG_OUTLET_SHORT_DEFN(rxctl, all);
HLOG_OUTLET_SHORT_DEFN(protocol, all);
HLOG_OUTLET_SHORT_DEFN(txctl, all);
HLOG_OUTLET_SHORT_DEFN(memreg, all);
HLOG_OUTLET_SHORT_DEFN(msg, all);
HLOG_OUTLET_SHORT_DEFN(payload, all);
HLOG_OUTLET_SHORT_DEFN(paybuf, all);
HLOG_OUTLET_SHORT_DEFN(paybuflist, paybuf);
HLOG_OUTLET_SHORT_DEFN(completion, all);

static const char fget_fput_service_name[] = "4242";

static state_t global_state;
static pthread_mutex_t workers_mtx = PTHREAD_MUTEX_INITIALIZER;
static worker_t workers[WORKERS_MAX];
static size_t nworkers_running;
static size_t nworkers_allocated;
static pthread_cond_t nworkers_cond = PTHREAD_COND_INITIALIZER;

static bool workers_assignment_suspended = false;

static struct {
    int signum;
    struct sigaction saved_action;
} siglist[] = {
  {.signum = SIGHUP}
, {.signum = SIGINT}
, {.signum = SIGQUIT}
, {.signum = SIGTERM}
};

static const struct {
    uint64_t rx;
    uint64_t tx;
} payload_access = {.rx = FI_RECV | FI_REMOTE_WRITE, .tx = FI_SEND};

static char *
completion_flags_to_string(const uint64_t flags, char * const buf,
    const size_t bufsize)
{
    char *next = buf;
    static const struct {
        uint64_t flag;
        const char *name;
    } flag_to_name[] = {
      {.flag = FI_RECV, .name = "recv"}
    , {.flag = FI_SEND, .name = "send"}
    , {.flag = FI_MSG, .name = "msg"}
    , {.flag = FI_RMA, .name = "rma"}
    , {.flag = FI_WRITE, .name = "write"}
    , {.flag = FI_COMPLETION, .name = "completion"}
    , {.flag = FI_DELIVERY_COMPLETE, .name = "delivery complete"}
    };
    size_t i;
    size_t bufleft = bufsize;
    uint64_t found = 0, residue;
    const char *delim = "<";

    if (bufsize < 1)
        return NULL;
    buf[0] = '\0';

    for (i = 0; i < arraycount(flag_to_name); i++) {
        uint64_t curflag = flag_to_name[i].flag;
        const char *name = flag_to_name[i].name;
        if ((flags & curflag) == 0)
            continue;
        found |= curflag;
        int nprinted = snprintf(next, bufleft, "%s%s", delim, name);
        delim = ",";
        if (nprinted < 0 || (size_t)nprinted >= bufleft)
            continue;
        next += nprinted;
        bufleft -= (size_t)nprinted;
    }
    residue = flags & ~found;
    while (residue != 0) {
        uint64_t oresidue = residue;
        residue = residue & (residue - 1);
        uint64_t lsb = oresidue ^ residue;
        int nprinted = snprintf(next, bufleft, "%s0x%" PRIx64, delim, lsb);
        delim = ",";
        if (nprinted < 0 || (size_t)nprinted >= bufleft)
            continue;
        next += nprinted;
        bufleft -= (size_t)nprinted;
    }
    if (next != buf)
        (void)snprintf(next, bufleft, ">");
    return buf;
}

static const uint64_t desired_rx_flags = FI_RECV | FI_MSG;
static const uint64_t desired_tx_flags = FI_SEND | FI_MSG;
static const uint64_t desired_wr_flags =
    FI_RMA | FI_WRITE | FI_COMPLETION | FI_DELIVERY_COMPLETE;

static uint64_t _Atomic next_key_pool = 512;

static volatile sig_atomic_t cancelled = 0;

static char txbuf[] =
    "If this message was received in error then please "
    "print it out and shred it.";

#define bailout_for_ofi_ret(ret, ...)                          \
        bailout_for_ofi_ret_impl(ret, __func__, __LINE__, __VA_ARGS__)

#define warn_about_ofi_ret(ret, ...)                          \
        warn_about_ofi_ret_impl(ret, __func__, __LINE__, __VA_ARGS__)

static void
warnv_about_ofi_ret_impl(int ret, const char *fn, int lineno,
    const char *fmt, va_list ap)
{
    fprintf(stderr, "%s.%d: ", fn, lineno);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, ": %s\n", fi_strerror(-ret));
}

static void
warn_about_ofi_ret_impl(int ret, const char *fn, int lineno,
    const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    warnv_about_ofi_ret_impl(ret, fn, lineno, fmt, ap);
    va_end(ap);
}

static void
bailout_for_ofi_ret_impl(int ret, const char *fn, int lineno,
    const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    warnv_about_ofi_ret_impl(ret, fn, lineno, fmt, ap);
    va_end(ap);

    exit(EXIT_FAILURE);
}

#if 0
static int
maxsize(size_t l, size_t r)
{
    return (l > r) ? l : r;
}
#endif

static int
minsize(size_t l, size_t r)
{
    return (l < r) ? l : r;
}

static inline bool
size_is_power_of_2(size_t size)
{
    return ((size - 1) & size) == 0;
}

static fifo_t *
fifo_create(size_t size)
{
    if (!size_is_power_of_2(size))
        return NULL;

    fifo_t *f = malloc(offsetof(fifo_t, hdr[0]) + sizeof(f->hdr[0]) * size);

    if (f == NULL)
        return NULL;

    f->insertions = f->removals = 0;
    f->index_mask = size - 1;

    return f;
}

static void
fifo_destroy(fifo_t *f)
{
    free(f);
}

static inline bufhdr_t *
fifo_get(fifo_t *f)
{
    assert(f->insertions >= f->removals);

    if (f->insertions == f->removals)
        return NULL;

    bufhdr_t *h = f->hdr[f->removals & (uint64_t)f->index_mask];
    f->removals++;

    return h;
}

static inline bufhdr_t *
fifo_peek(fifo_t *f)
{
    assert(f->insertions >= f->removals);

    if (f->insertions == f->removals)
        return NULL;

    return f->hdr[f->removals & (uint64_t)f->index_mask];
}

static inline bool
fifo_empty(fifo_t *f)
{
    return f->insertions == f->removals;
}

static inline bool
fifo_full(fifo_t *f)
{
    return f->insertions - f->removals == f->index_mask + 1;
}

static inline bool
fifo_put(fifo_t *f, bufhdr_t *h)
{
    assert(f->insertions - f->removals <= f->index_mask + 1);

    if (f->insertions - f->removals > f->index_mask)
        return false;

    f->hdr[f->insertions & (uint64_t)f->index_mask] = h;
    f->insertions++;

    return true;
}

static bufhdr_t *
buflist_get(buflist_t *bl)
{
    if (bl->nfull == 0)
        return NULL;

    return bl->buf[--bl->nfull];
}

static bool
buflist_put(buflist_t *bl, bufhdr_t *h)
{
    if (bl->nfull == bl->nallocated)
        return false;

    bl->buf[bl->nfull++] = h;
    return true;
}

static bool
session_init(session_t *s, cxn_t *c, terminal_t *t)
{
    memset(s, 0, sizeof(*s));

    s->cxn = c;
    s->terminal = t;

    if ((s->ready_for_cxn = fifo_create(64)) == NULL)
        return NULL;

    if ((s->ready_for_terminal = fifo_create(64)) == NULL) {
        fifo_destroy(s->ready_for_cxn);
        return NULL;
    }

    return s;
}

static void
keysource_init(keysource_t *s)
{
    memset(s, 0, sizeof(*s));
}

static uint64_t
keysource_next(keysource_t *s)
{
    if (s->next_key % 256 == 0) {
            s->next_key = atomic_fetch_add_explicit(&next_key_pool, 256,
                memory_order_relaxed);
    }

    return s->next_key++;
}

static bufhdr_t *
buf_alloc(size_t paylen)
{
    bufhdr_t *h;

    if ((h = malloc(offsetof(bytebuf_t, payload[0]) + paylen)) == NULL)
        return NULL;

    h->nallocated = paylen;
    h->nused = 0;
    h->raddr = 0;

    return h;
}

static void
buf_free(bufhdr_t *h)
{
    free(h);
}

static bytebuf_t *
bytebuf_alloc(size_t paylen)
{
    return (bytebuf_t *)buf_alloc(paylen);
}

static fragment_t *
fragment_alloc(void)
{
    bufhdr_t *h;
    fragment_t *f;

    if ((h = buf_alloc(sizeof(*f) - sizeof(bufhdr_t))) == NULL)
        return NULL;

    f = (fragment_t *)h;
    h->xfc.type = xft_fragment;

    return f;
}

static progbuf_t *
progbuf_alloc(void)
{
    bufhdr_t *h;
    progbuf_t *pb;

    if ((h = buf_alloc(sizeof(*pb) - sizeof(bufhdr_t))) == NULL)
        return NULL;

    pb = (progbuf_t *)h;
    h->xfc.type = xft_progress;

    return pb;
}

static vecbuf_t *
vecbuf_alloc(void)
{
    bufhdr_t *h;
    vecbuf_t *vb;

    if ((h = buf_alloc(sizeof(*vb) - sizeof(bufhdr_t))) == NULL)
        return NULL;

    vb = (vecbuf_t *)h;
    h->xfc.type = xft_vector;

    return vb;
}

static int
buf_mr_reg(struct fid_domain *dom, uint64_t access, uint64_t key,
    bufhdr_t *h)
{
    int rc;
    bytebuf_t *b = (bytebuf_t *)h;

    rc = fi_mr_reg(dom, b->payload, h->nallocated, access, 0, key, 0, &h->mr, NULL);

    if (rc != 0)
        return rc;

    h->desc = fi_mr_desc(h->mr);

    return 0;
}

static int
buf_mr_dereg(bufhdr_t *h)
{
    return fi_close(&h->mr->fid);
}

static void
vecbuf_free(vecbuf_t *vb)
{
    buf_free(&vb->hdr);
}

static bool
worker_paybuflist_replenish(worker_t *w, uint64_t access, buflist_t *bl)
{
    size_t i, paylen;
    int rc;

    if (bl->nfull >= bl->nallocated / 2)
        return true;

    size_t ntofill = bl->nallocated / 2 - bl->nfull;

    for (paylen = 0, i = bl->nfull; i < ntofill; i++) {
        bytebuf_t *buf;

        // paylen cycle: -> 23 -> 29 -> 31 -> 37 -> 23
        switch (paylen) {
        case 0:
        default:
            paylen = 23;
            break;
        case 23:
            paylen = 29;
            break;
        case 29:
            paylen = 31;
            break;
        case 31:
            paylen = 37;
            break;
        case 37:
            paylen = 23;
            break;
        }
        buf = bytebuf_alloc(paylen);
        if (buf == NULL)
            err(EXIT_FAILURE, "%s.%d: malloc", __func__, __LINE__);

        buf->hdr.xfc.type = xft_rdma_write;

        if (!global_state.reregister &&
            (rc = buf_mr_reg(global_state.domain, access,
                             keysource_next(&w->keys), &buf->hdr)) != 0) {
            warn_about_ofi_ret(rc, "fi_mr_reg");
            free(buf);
            break;
        }

        hlog_fast(paybuflist,
            "%s: pushing %zu-byte buffer", __func__, buf->hdr.nallocated);
        bl->buf[i] = &buf->hdr;
    }
    bl->nfull = i;

    return bl->nfull > 0;
}

static bytebuf_t *
worker_payload_txbuf_get(worker_t *w)
{
    bytebuf_t *b;

    while ((b = (bytebuf_t *)buflist_get(w->paybufs.tx)) == NULL &&
           worker_paybuflist_replenish(w, payload_access.tx, w->paybufs.tx))
        ;   // do nothing

    if (b != NULL)
        hlog_fast(paybuf, "%s: buf length %zu", __func__, b->hdr.nallocated);

    return b;
}

static bytebuf_t *
worker_payload_rxbuf_get(worker_t *w)
{
    bytebuf_t *b;

    while ((b = (bytebuf_t *)buflist_get(w->paybufs.rx)) == NULL &&
           worker_paybuflist_replenish(w, payload_access.rx, w->paybufs.rx))
        ;   // do nothing

    if (b != NULL)
        hlog_fast(paybuf, "%s: buf length %zu", __func__, b->hdr.nallocated);

    return b;
}

static size_t
fibonacci_iov_setup(void *_buf, size_t len, struct iovec *iov, size_t niovs)
{
    char *buf = _buf;
    ssize_t i;
    struct fibo {
        size_t prev, curr;
    } state = {.prev = 0, .curr = 1}; // Fibonacci sequence state

    if (niovs < 1 && len > 0)
        return -1;

    if (niovs > SSIZE_MAX)
        niovs = SSIZE_MAX;

    for (i = 0; len > 0 && i < niovs - 1; i++) {
        iov[i].iov_len = (state.curr < len) ? state.curr : len;
        iov[i].iov_base = buf;
        len -= iov[i].iov_len;
        buf += iov[i].iov_len;
        state = (struct fibo){.prev = state.curr,
                              .curr = state.prev + state.curr};
    }
    if (len > 0) {
        iov[i].iov_len = len;
        iov[i].iov_base = buf;
        i++;
    }
    return i;
}

/* Register the `niovs`-segment I/O vector `iov` using up to `niovs`
 * of the registrations, descriptors, and remote addresses in the
 * vectors `mrp`, `descp`, and `raddrp`, respectively.  Register no
 * more than `maxsegs` segments in a single `fi_mr_regv` call.
 */
static int
mr_regv_all(struct fid_domain *domain, const struct iovec *iov,
    size_t niovs, size_t maxsegs, uint64_t access, uint64_t offset,
    keysource_t *keys, uint64_t flags, struct fid_mr **mrp,
    void **descp, uint64_t *raddrp, void *context)
{
    int rc;
    size_t i, j, nregs = (niovs + maxsegs - 1) / maxsegs;
    size_t nleftover;

    for (nleftover = niovs, i = 0;
         i < nregs;
         iov += maxsegs, nleftover -= maxsegs, i++) {
        struct fid_mr *mr;
        uint64_t raddr = 0;

        size_t nsegs = minsize(nleftover, maxsegs);

        hlog_fast(memreg, "%zu remaining I/O vectors", nleftover);

        rc = fi_mr_regv(domain, iov, nsegs,
            access, offset, keysource_next(keys), flags, &mr, context);

        if (rc != 0)
            goto err;

        for (j = 0; j < nsegs; j++) {
            hlog_fast(memreg, "filling descriptor %zu", i * maxsegs + j);
            mrp[i * maxsegs + j] = mr;
            descp[i * maxsegs + j] = fi_mr_desc(mr);
            raddrp[i * maxsegs + j] = raddr;
            raddr += iov[j].iov_len;
        }
    }

    return 0;

err:
    for (j = 0; j < i; j++)
        (void)fi_close(&mrp[j]->fid);

    return rc;
}

static bufhdr_t *
rxctl_complete(rxctl_t *rc, const completion_t *cmpl)
{
    bufhdr_t *h;

    if ((cmpl->flags & desired_rx_flags) != desired_rx_flags &&
        !cmpl->xfc->cancelled) {
        errx(EXIT_FAILURE,
            "%s: expected flags %" PRIu64 ", received flags %" PRIu64,
            __func__, desired_rx_flags, cmpl->flags & desired_rx_flags);
    }

    if ((h = fifo_get(rc->posted)) == NULL) {
        hlog_fast(rxctl,
            "%s: received a message, but no Rx was posted", __func__);
        return NULL;
    }

    if (cmpl->xfc != &h->xfc) {
        errx(EXIT_FAILURE, "%s: expected context %p received %p",
            __func__, (void *)&h->xfc.ctx, (void *)cmpl->xfc);
    }

    h->nused = cmpl->len;

    return h;
}

static void
rxctl_post(cxn_t *c, rxctl_t *ctl, bufhdr_t *h)
{
    int rc;

    rc = fi_recvmsg(c->ep, &(struct fi_msg){
          .msg_iov = &(struct iovec){.iov_base = &((bytebuf_t *)h)->payload[0],
                                     .iov_len = h->nallocated}
        , .desc = &h->desc
        , .iov_count = 1
        , .addr = c->peer_addr
        , .context = &h->xfc.ctx
        , .data = 0
        }, FI_COMPLETION);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_recvmsg");

    (void)fifo_put(ctl->posted, h);
}

static void
fifo_cancel(struct fid_ep *ep, fifo_t *posted)
{
    bufhdr_t *first = NULL, *h;
    int rc;

    while ((h = fifo_peek(posted)) != NULL) {
        if (h == first)
            break;
        (void)fifo_get(posted);
        if (first == NULL)
            first = h;
        h->xfc.cancelled = 1;
        if ((rc = fi_cancel(&ep->fid, &h->xfc.ctx)) != 0)
            bailout_for_ofi_ret(rc, "fi_cancel");
        (void)fifo_put(posted, h);
    }
}

static void
rxctl_cancel(struct fid_ep *ep, rxctl_t *ctl)
{
    fifo_cancel(ep, ctl->posted);
}

static void
txctl_cancel(struct fid_ep *ep, txctl_t *ctl)
{
    fifo_cancel(ep, ctl->posted);
}

/* Process completed progress-message transmission.  Return 0 if no
 * completions occurred, 1 if any completion occurred, -1 on an
 * irrecoverable error.
 */
static int
txctl_complete(txctl_t *tc, const completion_t *cmpl)
{
    bufhdr_t *h;

    if ((cmpl->flags & desired_tx_flags) != desired_tx_flags &&
        !cmpl->xfc->cancelled) {
        errx(EXIT_FAILURE,
            "%s: expected flags %" PRIu64 ", received flags %" PRIu64,
            __func__, desired_rx_flags, cmpl->flags & desired_rx_flags);
    }

    if ((h = fifo_get(tc->posted)) == NULL) {
        hlog_fast(txctl,
            "%s: message Tx completed, but no Tx was posted", __func__);
        return -1;
    }

    if (cmpl->xfc != &h->xfc) {
        errx(EXIT_FAILURE, "%s: expected context %p received %p",
            __func__, (void *)&h->xfc.ctx, cmpl->xfc);
    }

    if (!buflist_put(tc->pool, h))
        errx(EXIT_FAILURE, "%s: buffer pool full", __func__);

    return 1;
}

static void
txctl_transmit(cxn_t *c, txctl_t *tc)
{
    bufhdr_t *h;

    while ((h = fifo_peek(tc->ready)) != NULL && !fifo_full(tc->posted)) {
        const int rc = fi_sendmsg(c->ep, &(struct fi_msg){
              .msg_iov = &(struct iovec){
                  .iov_base = ((bytebuf_t *)h)->payload
                , .iov_len = h->nused}
            , .desc = h->desc
            , .iov_count = 1
            , .addr = c->peer_addr
            , .context = &h->xfc.ctx
            , .data = 0
            }, FI_COMPLETION);

        if (rc == 0) {
            (void)fifo_get(tc->ready);
            (void)fifo_put(tc->posted, h);
        } else if (rc == -FI_EAGAIN) {
            break;
        } else if (rc < 0) {
            bailout_for_ofi_ret(rc, "fi_sendmsg");
        }
    }
}

static loop_control_t
rcvr_start(worker_t *w, session_t *s)
{
    rcvr_t *r = (rcvr_t *)s->cxn;
    size_t nleftover, nloaded;

    r->cxn.started = true;

    while (!fifo_full(r->progress.posted)) {
        progbuf_t *pb = progbuf_alloc();

        rxctl_post(&r->cxn, &r->progress, &pb->hdr);
    }

    for (nleftover = sizeof(txbuf), nloaded = 0; nleftover > 0; ) {
        bytebuf_t *b = worker_payload_rxbuf_get(w);

        if (b == NULL) {
            hlog_fast(err, "%s: could not get a buffer", __func__);
            return loop_error;
        }

        b->hdr.nused = minsize(nleftover, b->hdr.nallocated);
        nleftover -= b->hdr.nused;
        nloaded += b->hdr.nused;
        if (!fifo_put(s->ready_for_cxn, &b->hdr)) {
            hlog_fast(err, "%s: could not enqueue tx buffer", __func__);
            return loop_error;
        }
    }

    return loop_continue;
}

/* Return `loop_continue` if the source is producing more bytes, `loop_end` if
 * the source will produce no more bytes.
 */
static loop_control_t
source_trade(terminal_t *t, fifo_t *ready, fifo_t *completed)
{
    source_t *s = (source_t *)t;
    bufhdr_t *h;

    if (t->eof)
        return loop_end;

    while ((h = fifo_peek(ready)) != NULL && !fifo_full(completed)) {
        bytebuf_t *b = (bytebuf_t *)h;
        size_t len, ofs;

        if (s->idx == s->entirelen) {
            t->eof = true;
            return loop_end;
        }

        h->nused = minsize(s->entirelen - s->idx, h->nallocated);
        for (ofs = 0; ofs < h->nused; ofs += len) {
            size_t txbuf_ofs = (s->idx + ofs) % s->txbuflen;
            len = minsize(h->nused - ofs, s->txbuflen - txbuf_ofs);
            memcpy(&b->payload[ofs], &txbuf[txbuf_ofs], len);
            printf("%.*s", (int)len, &b->payload[ofs]);
            fflush(stdout);
        }

        (void)fifo_get(ready);
        (void)fifo_put(completed, h);

        s->idx += h->nused;
    }

    if (s->idx != s->entirelen)
        return loop_continue;

    t->eof = true;
    return loop_end;
}

/* Return `loop_continue` if the sink is accepting more bytes, `loop_error` if
 * unexpected bytes are on `ready`, `loop_end` if the sink expects no more
 * bytes.
 */
static loop_control_t
sink_trade(terminal_t *t, fifo_t *ready, fifo_t *completed)
{
    sink_t *s = (sink_t *)t;
    bufhdr_t *h;

    if (t->eof && !fifo_empty(ready))
        goto fail;

    while ((h = fifo_peek(ready)) != NULL && !fifo_full(completed)) {
        bytebuf_t *b = (bytebuf_t *)h;
        size_t len, ofs;

        if (h->nused + s->idx > s->entirelen)
            goto fail;

        for (ofs = 0; ofs < h->nused; ofs += len) {
            size_t txbuf_ofs = (s->idx + ofs) % s->txbuflen;
            len = minsize(h->nused - ofs, s->txbuflen - txbuf_ofs);
            printf("%.*s", (int)len, &b->payload[ofs]);
            fflush(stdout);
            if (memcmp(&b->payload[ofs], &txbuf[txbuf_ofs], len) != 0)
                goto fail;
        }

        (void)fifo_get(ready);
        (void)fifo_put(completed, h);
        s->idx += h->nused;
    }
    if (s->idx != s->entirelen)
        return loop_continue;

    t->eof = true;
    return loop_end;
fail:
    hlog_fast(payload, "unexpected received payload");
    return loop_error;
}

static bool
progbuf_is_wellformed(progbuf_t *pb)
{
    return pb->hdr.nused == sizeof(pb->msg);
}

/* Process completion vector-message reception.  Return 0 if no
 * completions occurred, 1 if any completion occurred, -1 on an
 * irrecoverable error.
 */
static int
rcvr_progress_rx_process(rcvr_t *r, const completion_t *cmpl)
{
    progbuf_t *pb;

    if ((pb = (progbuf_t *)rxctl_complete(&r->progress, cmpl)) == NULL)
        return -1;

    if (pb->hdr.xfc.cancelled) {
        buf_free(&pb->hdr);
        return 0;
    }

    if (!progbuf_is_wellformed(pb)) {
        rxctl_post(&r->cxn, &r->progress, &pb->hdr);
        return 0;
    }

    hlog_fast(msg, "%s: received progress message, %"
        PRIu64 " bytes filled, %" PRIu64 " bytes leftover.", __func__,
        pb->msg.nfilled, pb->msg.nleftover);

    r->nfull += pb->msg.nfilled;

    if (pb->msg.nleftover == 0) {
        hlog_fast(protocol, "%s: received remote EOF", __func__);
        r->cxn.eof.remote = true;
    }

    rxctl_post(&r->cxn, &r->progress, &pb->hdr);

    return 1;
}

/* Process completions.  Return 0 if no completions occurred, 1 if
 * any completion occurred, -1 on an irrecoverable error.
 */
static int
rcvr_cq_process(rcvr_t *r)
{
    struct fi_cq_msg_entry fcmpl;
    completion_t cmpl;
    ssize_t ncompleted;

    if ((ncompleted = fi_cq_read(r->cxn.cq, &fcmpl, 1)) == -FI_EAGAIN)
        return 0;

    if (ncompleted == -FI_EAVAIL) {
        struct fi_cq_err_entry e;
        char errbuf[256];
        char flagsbuf[256];
        ssize_t nfailed = fi_cq_readerr(r->cxn.cq, &e, 0);

        cmpl = (completion_t){
          .xfc = e.op_context
        , .len = 0
        , .flags = 0
        };

        if (e.err != FI_ECANCELED || !cmpl.xfc->cancelled) {
            hlog_fast(err, "%s: read %zd errors, %s", __func__, nfailed,
                fi_strerror(e.err));
            hlog_fast(err, "%s: context %p", __func__, (void *)e.op_context);
            hlog_fast(err, "%s: completion flags %" PRIx64 " expected %" PRIx64,
                __func__, e.flags, desired_rx_flags);
            hlog_fast(err, "%s: symbolic flags %s", __func__,
                completion_flags_to_string(e.flags, flagsbuf,
                sizeof(flagsbuf)));
            hlog_fast(err, "%s: provider error %s", __func__,
                fi_cq_strerror(r->cxn.cq, e.prov_errno, e.err_data, errbuf,
                    sizeof(errbuf)));
            return -1;
        }
    } else if (ncompleted < 0) {
        bailout_for_ofi_ret(ncompleted, "fi_cq_sread");
    } else if (ncompleted != 1) {
        errx(EXIT_FAILURE,
            "%s: expected 1 completion, read %zd", __func__, ncompleted);
    } else {
        cmpl = (completion_t){
          .xfc = fcmpl.op_context
        , .len = fcmpl.len
        , .flags = fcmpl.flags
        };
    }

    switch (cmpl.xfc->type) {
    case xft_progress:
        hlog_fast(completion, "%s: read a progress rx completion", __func__);
        return rcvr_progress_rx_process(r, &cmpl);
    case xft_vector:
        hlog_fast(completion, "%s: read a vector tx completion", __func__);
        return txctl_complete(&r->vec, &cmpl);
    default:
        hlog_fast(completion, "%s: unexpected xfer context type", __func__);
        return -1;
    }
}

static void
rcvr_vector_update(session_t *s, rcvr_t *r)
{
    bufhdr_t *h;
    vecbuf_t *vb;
    size_t i;
    int rc;

    /* Transmit vector. */

    if (r->cxn.eof.remote && !r->cxn.eof.local &&
        !fifo_full(r->vec.ready) &&
        (vb = (vecbuf_t *)buflist_get(r->vec.pool)) != NULL) {
        memset(vb->msg.iov, 0, sizeof(vb->msg.iov));
        vb->msg.niovs = 0;
        (void)fifo_put(r->vec.ready, &vb->hdr);
        r->cxn.eof.local = true;
        hlog_fast(protocol, "%s: enqueued local EOF", __func__);
    } else while (!fifo_full(r->vec.ready) && !fifo_empty(s->ready_for_cxn) &&
           (vb = (vecbuf_t *)buflist_get(r->vec.pool)) != NULL) {

        for (i = 0;
             i < arraycount(vb->msg.iov) &&
                 (h = fifo_get(s->ready_for_cxn)) != NULL;
             i++) {

            h->nused = 0;

            if (global_state.reregister &&
                (rc = buf_mr_reg(global_state.domain, payload_access.rx,
                                 keysource_next(&r->cxn.keys), h)) < 0)
                bailout_for_ofi_ret(rc, "payload memory registration failed");

            (void)fifo_put(r->tgtposted, h);

            vb->msg.iov[i].addr = 0;
            vb->msg.iov[i].len = h->nallocated;
            vb->msg.iov[i].key = fi_mr_key(h->mr);
        }
        vb->msg.niovs = i;
        vb->hdr.nused = (char *)&vb->msg.iov[i] - (char *)&vb->msg;

        (void)fifo_put(r->vec.ready, &vb->hdr);
    }
}

static void
rcvr_targets_read(session_t *s, rcvr_t *r)
{
    bufhdr_t *h;
    int rc;

    while (r->nfull > 0 &&
          (h = fifo_peek(r->tgtposted)) != NULL &&
          !fifo_full(s->ready_for_terminal)) {
        if (h->nused + r->nfull < h->nallocated) {
            h->nused += r->nfull;
            r->nfull = 0;
        } else {
            r->nfull -= (h->nallocated - h->nused);
            h->nused = h->nallocated;
            (void)fifo_get(r->tgtposted);

            if (global_state.reregister && (rc = fi_close(&h->mr->fid)) != 0)
                warn_about_ofi_ret(rc, "fi_close");

            (void)fifo_put(s->ready_for_terminal, h);
        }
    }

    /* The remote does not necessarily indicate EOF on an RDMA target
     * buffer boundary.  On EOF, take any partially-full (looking on the
     * bright side) RDMA buffer off of the queue of RDMA target buffers.
     */
    if (r->cxn.eof.remote && (h = fifo_peek(r->tgtposted)) != NULL &&
        h->nused != 0) {
        (void)fifo_get(r->tgtposted);

        if (global_state.reregister && (rc = fi_close(&h->mr->fid)) != 0)
            warn_about_ofi_ret(rc, "fi_close");

        (void)fifo_put(s->ready_for_terminal, h);
    }
}

static loop_control_t
rcvr_loop(worker_t *w, session_t *s)
{
    rcvr_t *r = (rcvr_t *)s->cxn;
    terminal_t *t = s->terminal;
    int rc;
    loop_control_t ctl;

    if (!r->cxn.started)
        return rcvr_start(w, s);

    if (rcvr_cq_process(r) == -1)
        goto fail;

    if (r->cxn.cancelled) {
        if (fifo_empty(r->progress.posted) &&
            fifo_empty(r->vec.posted))
            goto fail;
        return loop_continue;
    } else if (cancelled) {
        rxctl_cancel(r->cxn.ep, &r->progress);
        txctl_cancel(r->cxn.ep, &r->vec);
        r->cxn.cancelled = true;
        return loop_continue;
    }

    ctl = sink_trade(t, s->ready_for_terminal, s->ready_for_cxn);

    if (ctl == loop_error)
        goto fail;

    rcvr_vector_update(s, r);

    txctl_transmit(&r->cxn, &r->vec);

    rcvr_targets_read(s, r);

    if (t->eof && fifo_empty(s->ready_for_terminal) &&
        r->cxn.eof.remote && r->cxn.eof.local &&
        fifo_empty(r->vec.posted)) {
        if ((rc = fi_close(&r->cxn.ep->fid)) < 0)
            bailout_for_ofi_ret(rc, "fi_close");
        hlog_fast(close, "%s: closed.", __func__);
        return loop_end;
    }
    return loop_continue;
fail:
    if ((rc = fi_close(&r->cxn.ep->fid)) < 0)
        bailout_for_ofi_ret(rc, "fi_close");
    hlog_fast(close, "%s: closed.", __func__);
    return loop_error;
}

static loop_control_t
xmtr_start(worker_t *w, session_t *s)
{
    struct fi_cq_msg_entry completion;
    xmtr_t *x = (xmtr_t *)s->cxn;
    ssize_t ncompleted;
    int rc;

    x->cxn.started = true;

    while (!fifo_full(s->ready_for_terminal)) {
        bytebuf_t *b = worker_payload_txbuf_get(w);

        if (b == NULL)
            errx(EXIT_FAILURE, "%s: could not get a buffer", __func__);

        b->hdr.nused = 0;
        if (!fifo_put(s->ready_for_terminal, &b->hdr))
            errx(EXIT_FAILURE, "%s: could not enqueue tx buffer", __func__);
    }

    /* Post receive for connection acknowledgement. */
    x->ack.desc = fi_mr_desc(x->ack.mr);

    rc = fi_recvmsg(x->cxn.ep, &(struct fi_msg){
          .msg_iov = &(struct iovec){.iov_base = &x->ack.msg,
                                     .iov_len = sizeof(x->ack.msg)}
        , .desc = &x->ack.desc
        , .iov_count = 1
        , .addr = x->cxn.peer_addr
        , .context = NULL
        , .data = 0
        }, FI_COMPLETION);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_recvmsg");

    /* Transmit initial message. */
    while ((rc = fi_sendmsg(x->cxn.ep, &(struct fi_msg){
          .msg_iov = &(struct iovec){.iov_base = &x->initial.msg,
                                     .iov_len = sizeof(x->initial.msg)}
        , .desc = &x->initial.desc
        , .iov_count = 1
        , .addr = x->cxn.peer_addr
        , .context = NULL
        , .data = 0
        }, 0)) == -FI_EAGAIN) {

        /* Await reply to initial message: first vector message. */
        ncompleted = fi_cq_read(x->cxn.cq, &completion, 1);

        if (ncompleted == -FI_EAGAIN)
            continue;

        if (ncompleted < 0)
            bailout_for_ofi_ret(ncompleted, "fi_cq_sread");

        if (ncompleted != 1) {
            errx(EXIT_FAILURE,
                "%s: expected 1 completion, read %zd", __func__, ncompleted);
        }

        errx(EXIT_FAILURE,
            "%s: expected flags %" PRIu64 ", received flags %" PRIu64,
            __func__, desired_rx_flags, completion.flags & desired_rx_flags);
    }

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_sendmsg");

    /* Await reply to initial message: first ack message. */
    do {
        hlog_fast(tx_start, "%s: awaiting ack message reception", __func__);
        ncompleted = fi_cq_sread(x->cxn.cq, &completion, 1, NULL, -1);
    } while (ncompleted == -FI_EAGAIN);

    if (ncompleted < 0)
        bailout_for_ofi_ret(ncompleted, "fi_cq_sread");

    if (ncompleted != 1) {
        errx(EXIT_FAILURE,
            "%s: expected 1 completion, read %zd", __func__, ncompleted);
    }

    if ((completion.flags & desired_rx_flags) != desired_rx_flags) {
        errx(EXIT_FAILURE,
            "%s: expected flags %" PRIu64 ", received flags %" PRIu64,
            __func__, desired_rx_flags, completion.flags & desired_rx_flags);
    }

    if (completion.len != sizeof(x->ack.msg))
        errx(EXIT_FAILURE, "%s: ack is incorrect size", __func__);

    fi_addr_t oaddr = x->cxn.peer_addr;
    rc = fi_av_insert(x->cxn.av, x->ack.msg.addr, 1, &x->cxn.peer_addr,
        0, NULL);

    if (rc < 0) {
        bailout_for_ofi_ret(rc, "fi_av_insert dest_addr %p", x->ack.msg.addr);
    }

    rc = fi_av_remove(x->cxn.av, &oaddr, 1, 0);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_av_remove old dest_addr");

    while (!fifo_full(x->vec.posted)) {
        vecbuf_t *vb = vecbuf_alloc();

        rc = buf_mr_reg(global_state.domain, FI_RECV,
            keysource_next(&x->cxn.keys), &vb->hdr);

        if (rc < 0)
            bailout_for_ofi_ret(rc, "buffer memory registration failed");

        rxctl_post(&x->cxn, &x->vec, &vb->hdr);
    }

    return loop_continue;
}

typedef struct write_fully_params {
    struct fid_ep *ep;
    const struct iovec *iov_in;
    void **desc_in;
    struct iovec *iov_out;
    void **desc_out;
    size_t niovs;
    size_t *niovs_out;
    const struct fi_rma_iov *riov_in;
    struct fi_rma_iov *riov_out;
    size_t nriovs;
    size_t *nriovs_out;
    size_t len;
    size_t maxsegs;
    uint64_t flags;
    fi_addr_t addr;
    struct fi_context *context;
} write_fully_params_t;

static ssize_t
write_fully(const write_fully_params_t p)
{
    ssize_t rc;
    size_t i, j, nremaining;
    struct {
        size_t local;
        size_t remote;
    } maxsegs = {.local = minsize(p.maxsegs, p.niovs),
                 .remote = minsize(p.maxsegs, p.nriovs)},
      nsegs = {.local = 0, .remote = 0}, sumlen = {.local = 0, .remote = 0};

    for (i = 0; i < maxsegs.local; i++)
        sumlen.local += p.iov_in[i].iov_len;

    for (i = 0; i < maxsegs.remote; i++)
        sumlen.remote += p.riov_in[i].len;

    const size_t len = minsize(minsize(sumlen.local, sumlen.remote),
                               minsize(p.len, SSIZE_MAX));

    for (i = 0, nremaining = len; 0 < nremaining && i < maxsegs.local; i++) {
        p.iov_out[i] = p.iov_in[i];
        p.desc_out[i] = p.desc_in[i];
        if (p.iov_in[i].iov_len > nremaining) {
            p.iov_out[i].iov_len = nremaining;
            nremaining = 0;
        } else {
            nremaining -= p.iov_in[i].iov_len;
        }
    }

    nsegs.local = i;

    for (i = 0, nremaining = len; 0 < nremaining && i < maxsegs.remote; i++) {
        p.riov_out[i] = p.riov_in[i];
        if (p.riov_in[i].len > nremaining) {
            p.riov_out[i].len = nremaining;
            nremaining = 0;
        } else {
            nremaining -= p.riov_in[i].len;
        }
    }

    nsegs.remote = i;

    struct fi_msg_rma mrma = {
      .msg_iov = p.iov_out
    , .desc = p.desc_out
    , .iov_count = nsegs.local
    , .addr = p.addr
    , .rma_iov = p.riov_out
    , .rma_iov_count = nsegs.remote
    , .context = p.context
    , .data = 0
    };

    rc = fi_writemsg(p.ep, &mrma, p.flags);

    if (rc != 0)
        return rc;

    for (i = j = 0, nremaining = len; i < p.niovs; i++) {
        if (nremaining >= p.iov_in[i].iov_len) {
            nremaining -= p.iov_in[i].iov_len;
            continue;
        }
        p.desc_out[j] = p.desc_in[i];
        p.iov_out[j] = p.iov_in[i];
        if (nremaining > 0) {
            p.iov_out[j].iov_len -= nremaining;
            p.iov_out[j].iov_base = (char *)p.iov_out[j].iov_base + nremaining;
            nremaining = 0;
        }
        j++;
    }
    *p.niovs_out = j;

    for (i = j = 0, nremaining = len; i < p.nriovs; i++) {
        if (nremaining >= p.riov_in[i].len) {
            nremaining -= p.riov_in[i].len;
            continue;
        }
        p.riov_out[j] = p.riov_in[i];
        if (nremaining > 0) {
            p.riov_out[j].len -= nremaining;
            p.riov_out[j].addr += nremaining;
            nremaining = 0;
        }
        j++;
    }

    *p.nriovs_out = j;
    return len;
}

static bool
vecbuf_is_wellformed(vecbuf_t *vb)
{
    size_t len = vb->hdr.nused;

    static const ptrdiff_t least_vector_msglen =
        (char *)&vb->msg.iov[0] - (char *)&vb->msg;

    const size_t niovs_space = (len - least_vector_msglen) /
        sizeof(vb->msg.iov[0]);

    if (len < least_vector_msglen) {
        hlog_fast(err, "%s: expected >= %zu bytes, received %zu",
            __func__, least_vector_msglen, len);
    } else if ((len - least_vector_msglen) % sizeof(vb->msg.iov[0]) != 0) {
        hlog_fast(err,
            "%s: %zu-byte vector message did not end on vector boundary, "
            "disconnecting...", __func__, len);
    } else if (niovs_space < vb->msg.niovs) {
        hlog_fast(err,
            "%s: peer sent truncated vectors, disconnecting...", __func__);
    } else if (vb->msg.niovs > arraycount(vb->msg.iov)) {
        hlog_fast(err,
            "%s: peer sent too many vectors, disconnecting...", __func__);
    } else
        return true;

    return false;
}

static void
xmtr_vecbuf_unload(xmtr_t *x)
{
    vecbuf_t *vb;
    struct fi_rma_iov *riov;
    size_t i;

    if ((vb = (vecbuf_t *)fifo_peek(x->vec.rcvd)) == NULL)
        return;

    riov = (!x->phase) ? x->riov : x->riov2;

    if (!x->cxn.eof.remote && vb->msg.niovs == 0) {
        hlog_fast(protocol, "%s: received remote EOF", __func__);
        x->cxn.eof.remote = true;
    }

    for (i = x->next_riov;
         i < vb->msg.niovs && x->nriovs < arraycount(x->riov);
         i++) {
        hlog_fast(protocol, "%s: received vector %zu "
            "addr %" PRIu64 " len %" PRIu64 " key %" PRIx64,
            __func__, i, vb->msg.iov[i].addr, vb->msg.iov[i].len,
            vb->msg.iov[i].key);

        riov[x->nriovs++] = (struct fi_rma_iov){
          .len = vb->msg.iov[i].len
        , .addr = vb->msg.iov[i].addr
        , .key = vb->msg.iov[i].key
        };
    }

    if (i == vb->msg.niovs) {
        (void)fifo_get(x->vec.rcvd);
        rxctl_post(&x->cxn, &x->vec, &vb->hdr);
        x->next_riov = 0;
    } else
        x->next_riov = i;
}

/* Process completed vector-message reception.  Return 0 if no
 * completions occurred, 1 if any completion occurred, -1 on an
 * irrecoverable error.
 */
static int
xmtr_vector_rx_process(xmtr_t *x, const completion_t *cmpl)
{
    vecbuf_t *vb;

    if ((vb = (vecbuf_t *)rxctl_complete(&x->vec, cmpl)) == NULL)
        return -1;

    if (vb->hdr.xfc.cancelled) {
        buf_free(&vb->hdr);
        return 0;
    }

    if (!vecbuf_is_wellformed(vb)) {
        hlog_fast(err, "%s: rx'd malformed vector message", __func__);
        rxctl_post(&x->cxn, &x->vec, &vb->hdr);
        return 0;
    }

    if (!fifo_put(x->vec.rcvd, &vb->hdr))
        errx(EXIT_FAILURE, "%s: received vectors FIFO was full", __func__);

    return 1;
}

/* Process completions.  Return 0 if no completions occurred, 1 if
 * any completion occurred, -1 on an irrecoverable error.
 */
static int
xmtr_cq_process(xmtr_t *x, session_t *s, bool reregister)
{
    struct fi_cq_msg_entry fcmpl;
    completion_t cmpl;
    bufhdr_t *h;
    ssize_t ncompleted;

    if ((ncompleted = fi_cq_read(x->cxn.cq, &fcmpl, 1)) == -FI_EAGAIN)
        return 0;

    if (ncompleted == -FI_EAVAIL) {
        struct fi_cq_err_entry e;
        char errbuf[256];
        char flagsbuf[256];
        ssize_t nfailed = fi_cq_readerr(x->cxn.cq, &e, 0);

        cmpl = (completion_t){
          .xfc = e.op_context
        , .flags = 0
        , .len = 0
        };

        if (e.err != FI_ECANCELED || !cmpl.xfc->cancelled) {
            hlog_fast(err, "%s: read %zd errors, %s", __func__, nfailed,
                fi_strerror(e.err));
            hlog_fast(err, "%s: context %p", __func__, (void *)e.op_context);
            hlog_fast(err, "%s: completion flags %" PRIx64 " expected %" PRIx64,
                __func__, e.flags, desired_wr_flags);
            hlog_fast(err, "%s: symbolic flags %s", __func__,
                completion_flags_to_string(e.flags, flagsbuf,
                sizeof(flagsbuf)));
            hlog_fast(err, "%s: provider error %s", __func__,
                fi_cq_strerror(x->cxn.cq, e.prov_errno, e.err_data, errbuf,
                sizeof(errbuf)));
            return -1;
        }
    } else if (ncompleted < 0) {
        bailout_for_ofi_ret(ncompleted, "fi_cq_read");
    } else if (ncompleted != 1) {
        errx(EXIT_FAILURE,
            "%s: expected 1 completion, read %zd", __func__, ncompleted);
    } else {
        cmpl = (completion_t){
          .xfc = fcmpl.op_context
        , .flags = fcmpl.flags
        , .len = fcmpl.len
        };
    }

    cmpl.xfc->owner = xfo_program;

    switch (cmpl.xfc->type) {
    case xft_vector:
        hlog_fast(completion, "%s: read a vector rx completion", __func__);
        return xmtr_vector_rx_process(x, &cmpl);
    case xft_fragment:
    case xft_rdma_write:
        hlog_fast(completion, "%s: read an RDMA-write completion", __func__);
        /* If the head of `wrposted` is marked `xfo_program`, then dequeue the
         * txbuffers at the head of `wrposted` through the last one marked
         * `xfo_program`.
         */
        if ((h = fifo_peek(x->wrposted)) == NULL) {
            hlog_fast(err, "%s: no RDMA-write completions expected", __func__);
            return -1;
        }
        /* XXX This can fail if `s->ready_for_terminal` ever fills
         * to capacity, in the loop below.  That should not happen
         * unless we accidentally put more buffers into circulation
         * than there are slots in `s->ready_for_terminal`.
         */
        if ((h->xfc.place & xfp_first) == 0) {
            hlog_fast(err, "%s: expected `first` context at head", __func__);
            return -1;
        }
        while ((h = fifo_peek(x->wrposted)) != NULL &&
               h->xfc.owner == xfo_program &&
               h->xfc.type == xft_fragment) {
            fragment_t *f = (fragment_t *)h;
            (void)fifo_get(x->wrposted);

            assert(f->parent->xfc.nchildren > 0);
            f->parent->xfc.nchildren--;

            (void)buflist_put(x->fragment.pool, h);
        }
        while ((h = fifo_peek(x->wrposted)) != NULL &&
               h->xfc.owner == xfo_program &&
               h->xfc.type == xft_rdma_write &&
               h->xfc.nchildren == 0 &&
               !fifo_full(s->ready_for_terminal)) {
            int rc;

            (void)fifo_get(x->wrposted);

            if (reregister && (rc = fi_close(&h->mr->fid)) != 0)
                warn_about_ofi_ret(rc, "fi_close");

            x->bytes_progress += h->nused;
            (void)fifo_put(s->ready_for_terminal, h);
        }
        return 1;
    case xft_progress:
        hlog_fast(completion, "%s: read a progress tx completion", __func__);
        return txctl_complete(&x->progress, &cmpl);
    default:
        hlog_fast(completion, "%s: unexpected xfer context type", __func__);
        return -1;
    }
}

static bufhdr_t *
xmtr_buf_split(xmtr_t *x, bufhdr_t *parent, size_t len)
{
    bufhdr_t *h;
    fragment_t *f;

    assert(x->fragment.offset < parent->nused);
    assert(len < parent->nused - x->fragment.offset);

    if ((h = buflist_get(x->fragment.pool)) == NULL)
        errx(EXIT_FAILURE, "%s: out of fragment headers", __func__);

    f = (fragment_t *)h;

    h->raddr = x->fragment.offset;
    h->nused = len;
    h->nallocated = 0;
    h->mr = parent->mr;
    h->desc = parent->desc;
    f->parent = parent;

    parent->xfc.nchildren++;

    return h;
}

/* Take Tx buffers off of our queue while their cumulative length
 * is less than the sum length of RDMA targets that we can write
 * in one scatter-gather I/O, sum(0 <= i < maxriovs, riov[i].len).
 *
 * If the first Tx buffer is longer than the RDMA targets, and no more
 * RDMA buffers are expected to arrive, then fragment the Tx buffer
 * and write the fragments independently.
 *
 * Flag the first Tx buffer `xfp_first` and the last `xfp_last`
 * (first and last may be the same buffer).  Clear flags on the rest
 * of the Tx buffers.  Set the owner of the first to `xfo_nic`.
 *
 * Finally, perform one fi_writemsg using the context on the first
 * Tx buffer.
 */
static loop_control_t
xmtr_targets_write(session_t *s, xmtr_t *x)
{
    bufhdr_t *first_h, *h, *head, *last_h = NULL;
    const size_t maxriovs = minsize(global_state.rma_maxsegs, x->nriovs);
    size_t i, len, maxbytes, niovs, niovs_out = 0, nriovs_out = 0;
    ssize_t nwritten, rc, total;

    for (maxbytes = 0, i = 0; i < maxriovs; i++)
        maxbytes += ((!x->phase) ? x->riov : x->riov2)[i].len;

    /* If x->nriovs < global_state.rma_maxsegs, then more RDMA vectors will
     * arrive, so there is no need to fragment.
     */
    const bool riovs_maxed_out = x->nriovs >= global_state.rma_maxsegs;

    for (i = 0, total = 0, first_h = last_h = NULL;
         i < maxriovs &&
             (head = fifo_peek(s->ready_for_cxn)) != NULL &&
             total < maxbytes && !fifo_full(x->wrposted);
         i++, last_h = h, total += len) {
        const bool oversize_load =
            head->nused - x->fragment.offset > maxbytes - total;

        hlog_fast(write,
            "%s: head %p nchildren %" PRIu32 " offset %zu nused %zu "
            "total %zu maxbytes %zu nriovs %zu maxsegs %zu",
            __func__, (void *)head, head->xfc.nchildren,
            x->fragment.offset, head->nused, total, maxbytes,
            x->nriovs, global_state.rma_maxsegs);

        /* Fragment oversize loads unless more RDMA vectors will arrive. */
        if (oversize_load && !riovs_maxed_out)
            break;

        if (oversize_load)
            len = maxbytes - total;
        else
            len = head->nused - x->fragment.offset;

        if (x->fragment.offset == 0)
            head->xfc.nchildren = 0;

        if (global_state.reregister && x->fragment.offset == 0 &&
            (rc = buf_mr_reg(global_state.domain, payload_access.tx,
                             keysource_next(&x->cxn.keys), head)) < 0)
            bailout_for_ofi_ret(rc, "payload memory registration failed");

        if (oversize_load) {
            h = xmtr_buf_split(x, head, len);
        } else {
            (void)fifo_get(s->ready_for_cxn);
            h = head;
        }

        (void)fifo_put(x->wrposted, h);

        if (last_h == NULL)
            first_h = h;

        h->xfc.owner = xfo_program;
        h->xfc.place = 0;

        bytebuf_t *b = (bytebuf_t *)head;

        ((!x->phase) ? x->payload.iov : x->payload.iov2)[i] = (struct iovec){
          .iov_len = len
        , .iov_base = &b->payload[x->fragment.offset]
        };
        ((!x->phase) ? x->payload.desc : x->payload.desc2)[i] = h->desc;
        if (oversize_load) {
            x->fragment.offset += len;
            assert(x->fragment.offset < head->nused);
        } else {
            x->fragment.offset = 0;
        }
    }
    niovs = i;

    if (first_h != NULL) {
        first_h->xfc.owner = xfo_nic;
        first_h->xfc.place = xfp_first;
        last_h->xfc.place |= xfp_last;

        write_fully_params_t p = {.ep = x->cxn.ep,
            .iov_in = (!x->phase) ? x->payload.iov : x->payload.iov2,
            .desc_in = (!x->phase) ? x->payload.desc : x->payload.desc2,
            .iov_out = (!x->phase) ? x->payload.iov2 : x->payload.iov,
            .desc_out = (!x->phase) ? x->payload.desc2 : x->payload.desc,
            .niovs = niovs,
            .niovs_out = &niovs_out,
            .riov_in = (!x->phase) ? x->riov : x->riov2,
            .riov_out = (!x->phase) ? x->riov2 : x->riov,
            .nriovs = x->nriovs,
            .nriovs_out = &nriovs_out,
            .len = total,
            .maxsegs = maxriovs,
            .flags = FI_COMPLETION | FI_DELIVERY_COMPLETE,
            .context = &first_h->xfc.ctx,
            .addr = x->cxn.peer_addr};

        nwritten = write_fully(p);

        if (nwritten < 0)
            bailout_for_ofi_ret(nwritten, "write_fully");

        if (nwritten != total || niovs_out != 0) {
            hlog_fast(err, "%s: local I/O vectors were partially written, "
                "nwritten %zu total %zu niovs_out %zu", __func__, nwritten,
                total, niovs_out);
            return loop_error;
        }

        x->nriovs = nriovs_out;

        x->phase = !x->phase;
    }
    return loop_continue;
}

static void
xmtr_progress_update(session_t *s, xmtr_t *x)
{
    progbuf_t *pb;

    /* If the terminal reached EOF, ready_for_cxn and wrposted
     * are empty, and nleftover == 0 has not previously been sent
     * (!x->cxn.eof.local), then send nleftover == 0; on a successful
     * transmission, set x->cxn.eof.local to true.
     */
    bool reached_eof = (s->terminal->eof &&
        fifo_empty(s->ready_for_cxn) && fifo_empty(x->wrposted) &&
        !x->cxn.eof.local);

    if (x->bytes_progress == 0 && !reached_eof)
        return;

    if (fifo_full(x->progress.ready))
        return;

    if ((pb = (progbuf_t *)buflist_get(x->progress.pool)) == NULL)
        return;

    pb->hdr.xfc.owner = xfo_nic;
    pb->hdr.nused = pb->hdr.nallocated;

    pb->msg.nfilled = x->bytes_progress;
    pb->msg.nleftover = reached_eof ? 0 : 1;

    hlog_fast(protocol, "%s: sending progress message, %"
        PRIu64 " filled, %" PRIu64 " leftover", __func__,
        pb->msg.nfilled, pb->msg.nleftover);

    x->bytes_progress = 0;

    (void)fifo_put(x->progress.ready, &pb->hdr);

    if (reached_eof) {
        hlog_fast(protocol, "%s: enqueued local EOF", __func__);
        x->cxn.eof.local = true;
    }
}

static loop_control_t
xmtr_loop(worker_t *w, session_t *s)
{
    vecbuf_t *vb;
    xmtr_t *x = (xmtr_t *)s->cxn;
    ssize_t rc;

    if (!x->cxn.started)
        return xmtr_start(w, s);

    if (xmtr_cq_process(x, s, global_state.reregister) == -1)
        goto fail;

    if (x->cxn.cancelled) {
        if (fifo_empty(x->progress.posted) &&
            fifo_empty(x->vec.posted) &&
            fifo_empty(x->wrposted))
            goto fail;
        return loop_continue;
    } else if (cancelled) {
        txctl_cancel(x->cxn.ep, &x->progress);
        rxctl_cancel(x->cxn.ep, &x->vec);
        fifo_cancel(x->cxn.ep, x->wrposted);
        x->cxn.cancelled = true;
        return loop_continue;
    }

    xmtr_vecbuf_unload(x);

    if (source_trade(s->terminal, s->ready_for_terminal, s->ready_for_cxn) ==
        loop_error)
        goto fail;

    if (xmtr_targets_write(s, x) == loop_error)
        goto fail;

    xmtr_progress_update(s, x);

    txctl_transmit(&x->cxn, &x->progress);

    if (!(s->terminal->eof && fifo_empty(s->ready_for_cxn) &&
        fifo_empty(x->wrposted) && x->bytes_progress == 0 &&
        x->cxn.eof.local))
        return loop_continue;

    /* Hunt for remote EOF. */
    while (!x->cxn.eof.remote &&
           (vb = (vecbuf_t *)fifo_get(x->vec.rcvd)) != NULL) {
        if (vb->msg.niovs == 0)
            x->cxn.eof.remote = true;
        buf_mr_dereg(&vb->hdr);
        vecbuf_free(vb);
    }

    if (x->cxn.eof.remote && fifo_empty(x->progress.posted)) {
        if ((rc = fi_close(&x->cxn.ep->fid)) < 0)
            bailout_for_ofi_ret(rc, "fi_close");
        hlog_fast(close, "%s: closed.", __func__);
        return loop_end;
    }

    return loop_continue;
fail:
    if ((rc = fi_close(&x->cxn.ep->fid)) < 0)
        bailout_for_ofi_ret(rc, "fi_close");
    hlog_fast(close, "%s: closed.", __func__);
    return loop_error;
}

static loop_control_t
cxn_loop(worker_t *w, session_t *s)
{
    hlog_fast(cxn_loop, "%s: going around", __func__);
    return s->cxn->loop(w, s);
}

static void
worker_run_loop(worker_t *self)
{
    size_t half, i;

    for (half = 0; half < 2; half++) {
        void *context;
        pthread_mutex_t *mtx = &self->mtx[half];
        int rc;

        if (pthread_mutex_trylock(mtx) == EBUSY)
            continue;

        if ((rc = fi_poll(self->pollset[half], &context, 1)) < 0) {
            (void)pthread_mutex_unlock(mtx);
            bailout_for_ofi_ret(rc, "fi_poll");
        }

        /* Find a non-empty session slot and go once through
         * connection & terminal loops.
         */
        for (i = 0; i < arraycount(self->session) / 2; i++) {
            session_t *s;
            cxn_t *c, **cp;

            s = &self->session[half * arraycount(self->session) / 2 + i];
            cp = &s->cxn;

            // skip empty slots
            if ((c = *cp) == NULL)
                continue;

            // continue at next cxn_t if `c` did not exit
            switch (cxn_loop(self, s)) {
            case loop_continue:
                continue;
            case loop_end:
                break;
            case loop_error:
                self->failed = true;
                break;
            }

            *cp = NULL;

            if ((rc = fi_poll_del(self->pollset[half], &c->cq->fid, 0)) != 0) {
                warn_about_ofi_ret(rc, "fi_poll_del");
                continue;
            }
            atomic_fetch_add_explicit(&self->nsessions[half], -1,
                memory_order_relaxed);
        }

        (void)pthread_mutex_unlock(mtx);
    }
}

static bool
worker_is_idle(worker_t *self)
{
    const ptrdiff_t self_idx = self - &workers[0];
    size_t half, nlocked;

    if (self->nsessions[0] != 0 || self->nsessions[1] != 0)
        return false;

    if (self_idx + 1 !=
            atomic_load_explicit(&nworkers_running, memory_order_relaxed))
        return false;

    if (pthread_mutex_trylock(&workers_mtx) == EBUSY)
        return false;

    for (nlocked = 0; nlocked < 2; nlocked++) {
        if (pthread_mutex_trylock(&self->mtx[nlocked]) == EBUSY)
            break;
    }

    bool idle = (nlocked == 2 &&
                 self->nsessions[0] == 0 && self->nsessions[1] == 0 &&
                 self_idx + 1 == nworkers_running);

    if (idle) {
        nworkers_running--;
        pthread_cond_signal(&nworkers_cond);
    }

    for (half = 0; half < nlocked; half++)
        (void)pthread_mutex_unlock(&self->mtx[half]);

    (void)pthread_mutex_unlock(&workers_mtx);

    return idle;
}

static void
worker_idle_loop(worker_t *self)
{
    const ptrdiff_t self_idx = self - &workers[0];

    (void)pthread_mutex_lock(&workers_mtx);
    while (nworkers_running <= self_idx && !self->cancelled)
        pthread_cond_wait(&self->sleep, &workers_mtx);
    (void)pthread_mutex_unlock(&workers_mtx);
}

static void *
worker_outer_loop(void *arg)
{
    worker_t *self = arg;

    while (!self->cancelled) {
        worker_idle_loop(self);
        do {
            worker_run_loop(self);
        } while (!worker_is_idle(self) && !self->cancelled);
    }
    return NULL;
}

static void
worker_paybuflist_destroy(worker_t *w, buflist_t *bl)
{
    size_t i;
    int rc;

    for (i = 0; i < bl->nfull; i++) {
        bufhdr_t *h = bl->buf[i];

        if (!global_state.reregister && (rc = fi_close(&h->mr->fid)) != 0)
            warn_about_ofi_ret(rc, "fi_close");

        free(h);
    }
    bl->nfull = bl->nallocated = 0;
    free(bl);
}

static buflist_t *
buflist_create(size_t n)
{
    buflist_t *bl = malloc(offsetof(buflist_t, buf) + sizeof(bl->buf[0]) * n);

    if (bl == NULL)
        return NULL;

    bl->nallocated = n;
    bl->nfull = 0;

    return bl;
}

static buflist_t *
worker_paybuflist_create(worker_t *w, uint64_t access)
{
    buflist_t *bl = buflist_create(16);

    if (bl == NULL)
        return NULL;

    if (!worker_paybuflist_replenish(w, access, bl)) {
        worker_paybuflist_destroy(w, bl);
        return NULL;
    }

    return bl;
}

static void
worker_init(worker_t *w)
{
    struct fi_poll_attr attr = {.flags = 0};
    int rc;
    size_t i;

    w->cancelled = false;
    w->failed = false;
    keysource_init(&w->keys);

    if ((rc = pthread_cond_init(&w->sleep, NULL)) != 0) {
        errx(EXIT_FAILURE, "%s.%d: pthread_cond_init: %s", __func__, __LINE__,
            strerror(rc));
    }

    for (i = 0; i < arraycount(w->mtx); i++) {
        if ((rc = pthread_mutex_init(&w->mtx[i], NULL)) != 0) {
            errx(EXIT_FAILURE, "%s.%d: pthread_mutex_init: %s",
                __func__, __LINE__, strerror(rc));
        }
        if ((rc = fi_poll_open(global_state.domain, &attr,
                               &w->pollset[i])) != 0)
            bailout_for_ofi_ret(rc, "fi_poll_open");
    }
    for (i = 0; i < arraycount(w->session); i++)
        w->session[i] = (session_t){.cxn = NULL, .terminal = NULL};

    w->paybufs.rx = worker_paybuflist_create(w, payload_access.rx);
    w->paybufs.tx = worker_paybuflist_create(w, payload_access.tx);
}

static void
worker_launch(worker_t *w)
{
    int i, rc;
    sigset_t oldset, blockset;

    if ((rc = sigemptyset(&blockset)) == -1) {
        err(EXIT_FAILURE, "%s.%d: sigfillset", __func__, __LINE__);
    }

    for (i = 0; i < arraycount(siglist); i++) {
        if (sigaddset(&blockset, siglist[i].signum) == -1)
            err(EXIT_FAILURE, "%s.%d: sigaddset", __func__, __LINE__);
    }

    if ((rc = pthread_sigmask(SIG_BLOCK, &blockset, &oldset)) != 0) {
        errx(EXIT_FAILURE, "%s.%d: pthread_sigmask: %s", __func__, __LINE__,
            strerror(rc));
    }

    if ((rc = pthread_create(&w->thd, NULL, worker_outer_loop, w)) != 0) {
            errx(EXIT_FAILURE, "%s.%d: pthread_create: %s",
                __func__, __LINE__, strerror(rc));
    }

    if ((rc = pthread_sigmask(SIG_SETMASK, &oldset, NULL)) != 0) {
        errx(EXIT_FAILURE, "%s.%d: pthread_sigmask: %s", __func__, __LINE__,
            strerror(rc));
    }
}

#if 0
static void
worker_teardown(worker_t *w)
{
    int rc;
    size_t i;

    for (i = 0; i < arraycount(w->mtx); i++) {
        if ((rc = pthread_mutex_destroy(&w->mtx[i])) != 0) {
            errx(EXIT_FAILURE, "%s.%d: pthread_mutex_destroy: %s",
                __func__, __LINE__, strerror(rc));
        }
        if ((rc = fi_close(&w->pollset[i]->fid)) != 0)
            bailout_for_ofi_ret(rc, "fi_close");
    }
    for (i = 0; i < arraycount(w->session); i++)
        assert(w->session[i].cxn == NULL && w->session[i].terminal == NULL);

    if ((rc = pthread_cond_destroy(&w->sleep)) != 0) {
        errx(EXIT_FAILURE, "%s.%d: pthread_cond_destroy: %s",
            __func__, __LINE__, strerror(rc));
    }
    // TBD release buffers and free-buffer reservior.
}
#endif

static worker_t *
worker_create(void)
{
    worker_t *w;

    (void)pthread_mutex_lock(&workers_mtx);
    w = (nworkers_allocated < arraycount(workers))
        ? &workers[nworkers_allocated++]
        : NULL;
    if (w != NULL)
        worker_init(w);
    (void)pthread_mutex_unlock(&workers_mtx);

    if (w != NULL)
        worker_launch(w);

    return w;
}

static void
workers_initialize(void)
{
}

static bool
worker_assign_session(worker_t *w, session_t *s)
{
    size_t half, i;
    int rc;

    for (half = 0; half < 2; half++) {
        pthread_mutex_t *mtx = &w->mtx[half];

        if (pthread_mutex_trylock(mtx) == EBUSY)
            continue;

        // find an empty receiver slot
        for (i = 0; i < arraycount(w->session) / 2; i++) {
            session_t *slot =
                &w->session[half * arraycount(w->session) / 2 + i];

            if (slot->cxn != NULL)
                continue;

            rc = fi_poll_add(w->pollset[half], &s->cxn->cq->fid, 0);
            if (rc != 0) {
                warn_about_ofi_ret(rc, "fi_poll_add");
                continue;
            }
            atomic_fetch_add_explicit(&w->nsessions[half], 1,
                memory_order_relaxed);
            *slot = *s;
            (void)pthread_mutex_unlock(mtx);
            return true;
        }
        (void)pthread_mutex_unlock(mtx);
    }
    return false;
}

/* Try to allocate `c` to an active worker, least active, first.
 * Caller must hold `workers_mtx`.
 */
static worker_t *
workers_assign_session_to_running(session_t *s)
{
    size_t iplus1;

    for (iplus1 = nworkers_running; 0 < iplus1; iplus1--) {
        size_t i = iplus1 - 1;
        worker_t *w = &workers[i];
        if (worker_assign_session(w, s))
            return w;
    }
    return NULL;
}

/* Try to assign `c` to the next idle worker servicing `dom`.
 * Caller must hold `workers_mtx`.
 */
static worker_t *
workers_assign_session_to_idle(session_t *s)
{
    size_t i;

    if ((i = nworkers_running) < nworkers_allocated) {
        worker_t *w = &workers[i];
        if (worker_assign_session(w, s))
            return w;
    }
    return NULL;
}

/* Try to wake the first idle worker.
 *
 * Caller must hold `workers_mtx`.
 */
static void
workers_wake(worker_t *w)
{
    assert(&workers[nworkers_running] == w);
    nworkers_running++;
    pthread_cond_signal(&w->sleep);
}

static worker_t *
workers_assign_session(session_t *s)
{
    worker_t *w;

    do {
        (void)pthread_mutex_lock(&workers_mtx);

        if (workers_assignment_suspended) {
            (void)pthread_mutex_unlock(&workers_mtx);
            return NULL;
        }

        if ((w = workers_assign_session_to_running(s)) != NULL)
            ;
        else if ((w = workers_assign_session_to_idle(s)) != NULL)
            workers_wake(w);
        (void)pthread_mutex_unlock(&workers_mtx);
    } while (w == NULL && (w = worker_create()) != NULL);

    return w;
}

static int
workers_join_all(void)
{
    int code = EXIT_SUCCESS;
    size_t i;

    (void)pthread_mutex_lock(&workers_mtx);

    workers_assignment_suspended = true;

    while (nworkers_running > 0) {
        pthread_cond_wait(&nworkers_cond, &workers_mtx);
    }

    for (i = 0; i < nworkers_allocated; i++) {
        worker_t *w = &workers[i];
        w->cancelled = true;
        pthread_cond_signal(&w->sleep);
    }

    (void)pthread_mutex_unlock(&workers_mtx);

    for (i = 0; i < nworkers_allocated; i++) {
        worker_t *w = &workers[i];
        int rc;

        if ((rc = pthread_join(w->thd, NULL)) != 0) {
                errx(EXIT_FAILURE, "%s.%d: pthread_join: %s",
                    __func__, __LINE__, strerror(rc));
        }
        if (w->failed)
            code = EXIT_FAILURE;
    }
    return code;
}

static void
cxn_init(cxn_t *c, struct fid_av *av,
    loop_control_t (*loop)(worker_t *, session_t *))
{
    memset(c, 0, sizeof(*c));
    c->loop = loop;
    c->av = av;
    c->started = false;
    c->cancelled = false;
    c->eof.local = c->eof.remote = false;
    keysource_init(&c->keys);
}

static void
xmtr_memory_init(xmtr_t *x)
{
    const size_t txbuflen = strlen(txbuf);
    int rc;

    rc = fi_mr_reg(global_state.domain, &x->initial.msg, sizeof(x->initial.msg),
        FI_SEND, 0, keysource_next(&global_state.keys), 0, &x->initial.mr,
        NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_mr_reg");

    rc = fi_mr_reg(global_state.domain, &x->ack.msg, sizeof(x->ack.msg),
        FI_RECV, 0, keysource_next(&global_state.keys), 0, &x->ack.mr, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_mr_reg");

    rc = fi_mr_reg(global_state.domain, txbuf, txbuflen,
        FI_WRITE, 0, keysource_next(&global_state.keys), 0, x->payload.mr,
        NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_mr_reg");
}

static void
xmtr_init(xmtr_t *x, struct fid_av *av)
{
    const size_t nbufs = 16;
    const size_t maxposted = 64;
    size_t i;

    memset(x, 0, sizeof(*x));

    x->next_riov = 0;
    x->fragment.offset = 0;
    x->phase = false;
    x->bytes_progress = 0;

    cxn_init(&x->cxn, av, xmtr_loop);
    xmtr_memory_init(x);
    if ((x->wrposted = fifo_create(maxposted)) == NULL) {
        errx(EXIT_FAILURE,
            "%s: could not create posted RDMA writes FIFO", __func__);
    }
    if ((x->vec.posted = fifo_create(64)) == NULL) {
        errx(EXIT_FAILURE,
            "%s: could not create posted vectors FIFO", __func__);
    }
    if ((x->vec.rcvd = fifo_create(64)) == NULL) {
        errx(EXIT_FAILURE,
            "%s: could not create received vectors FIFO", __func__);
    }

    if ((x->progress.ready = fifo_create(64)) == NULL) {
        errx(EXIT_FAILURE,
            "%s: could not create ready progress-buffers FIFO", __func__);
    }

    if ((x->progress.posted = fifo_create(64)) == NULL) {
        errx(EXIT_FAILURE,
            "%s: could not create posted progress-buffers FIFO", __func__);
    }

    if ((x->progress.pool = buflist_create(nbufs)) == NULL) {
        errx(EXIT_FAILURE,
            "%s: could not create progress-message tx buffer pool", __func__);
    }

    if ((x->fragment.pool = buflist_create(maxposted)) == NULL) {
        errx(EXIT_FAILURE,
            "%s: could not create fragment header pool", __func__);
    }

    for (i = 0; i < maxposted; i++) {
        fragment_t *f = fragment_alloc();

        if (!buflist_put(x->fragment.pool, &f->hdr))
            errx(EXIT_FAILURE, "%s: fragment pool full", __func__);
    }

    for (i = 0; i < nbufs; i++) {
        progbuf_t *pb = progbuf_alloc();
        int rc;

        rc = buf_mr_reg(global_state.domain, FI_SEND,
            keysource_next(&global_state.keys), &pb->hdr);

        if (rc != 0) {
            warn_about_ofi_ret(rc, "fi_mr_reg");
            buf_free(&pb->hdr);
            break;
        }
        if (!buflist_put(x->progress.pool, &pb->hdr))
            errx(EXIT_FAILURE, "%s: progress buffer pool full", __func__);
    }
}

static void
terminal_init(terminal_t *t,
    loop_control_t (*trade)(terminal_t *, fifo_t *, fifo_t *))
{
    t->trade = trade;
    t->eof = false;
}

static void
sink_init(sink_t *s)
{
    memset(s, 0, sizeof(*s));
    terminal_init(&s->terminal, sink_trade);
    s->txbuflen = strlen(txbuf);
    s->entirelen = s->txbuflen * (size_t)10000;
    s->idx = 0;
}

static void
source_init(source_t *s)
{
    memset(s, 0, sizeof(*s));
    terminal_init(&s->terminal, source_trade);
    s->txbuflen = strlen(txbuf);
    s->entirelen = s->txbuflen * (size_t)10000;
    s->idx = 0;
}

static void
rcvr_memory_init(rcvr_t *r)
{
    int rc;

    r->initial.niovs = fibonacci_iov_setup(&r->initial.msg,
        sizeof(r->initial.msg), r->initial.iov, global_state.rx_maxsegs);

    if (r->initial.niovs < 1) {
        errx(EXIT_FAILURE, "%s: unexpected I/O vector length %zd",
            __func__, r->initial.niovs);
    }

    rc = mr_regv_all(global_state.domain, r->initial.iov, r->initial.niovs,
        minsize(2, global_state.mr_maxsegs), FI_RECV, 0, &global_state.keys, 0,
        r->initial.mr, r->initial.desc, r->initial.raddr, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "mr_regv_all");

    r->ack.niovs = fibonacci_iov_setup(&r->ack.msg,
        sizeof(r->ack.msg), r->ack.iov, global_state.rx_maxsegs);

    if (r->ack.niovs < 1) {
        errx(EXIT_FAILURE, "%s: unexpected I/O vector length %zd",
            __func__, r->ack.niovs);
    }

    rc = mr_regv_all(global_state.domain, r->ack.iov, r->ack.niovs,
        minsize(2, global_state.mr_maxsegs), FI_RECV, 0, &global_state.keys, 0,
        r->ack.mr, r->ack.desc, r->ack.raddr, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "mr_regv_all");
}

static void
rcvr_init(rcvr_t *r, struct fid_av *av)
{
    const size_t nbufs = 16;
    size_t i;

    memset(r, 0, sizeof(*r));

    cxn_init(&r->cxn, av, rcvr_loop);
    rcvr_memory_init(r);

    if ((r->tgtposted = fifo_create(64)) == NULL) {
        errx(EXIT_FAILURE,
            "%s: could not create RDMA targets FIFO", __func__);
    }

    if ((r->progress.posted = fifo_create(64)) == NULL) {
        errx(EXIT_FAILURE,
            "%s: could not create posted vectors FIFO", __func__);
    }
    if ((r->progress.rcvd = fifo_create(64)) == NULL) {
        errx(EXIT_FAILURE,
            "%s: could not create received vectors FIFO", __func__);
    }

    if ((r->vec.ready = fifo_create(64)) == NULL) {
        errx(EXIT_FAILURE,
            "%s: could not create ready vectors FIFO", __func__);
    }

    if ((r->vec.posted = fifo_create(64)) == NULL) {
        errx(EXIT_FAILURE,
            "%s: could not create posted vectors FIFO", __func__);
    }

    if ((r->vec.pool = buflist_create(nbufs)) == NULL) {
        errx(EXIT_FAILURE,
            "%s: could not create vector-message tx buffer pool", __func__);
    }

    for (i = 0; i < nbufs; i++) {
        vecbuf_t *vb = vecbuf_alloc();
        int rc;

        rc = buf_mr_reg(global_state.domain, FI_SEND,
            keysource_next(&global_state.keys), &vb->hdr);

        if (rc != 0) {
            warn_about_ofi_ret(rc, "fi_mr_reg");
            vecbuf_free(vb);
            break;
        }
        if (!buflist_put(r->vec.pool, &vb->hdr))
            errx(EXIT_FAILURE, "%s: vector buffer pool full", __func__);
    }
}

static int
get(void)
{
    struct fi_av_attr av_attr = {
      .type = FI_AV_UNSPEC
    , .rx_ctx_bits = 0
    , .count = 0
    , .ep_per_node = 0
    , .name = NULL
    , .map_addr = NULL
    , .flags = 0
    };
    struct fi_cq_attr cq_attr = {
      .size = 128
    , .flags = 0
    , .format = FI_CQ_FORMAT_MSG
    , .wait_obj = FI_WAIT_UNSPEC
    , .signaling_vector = 0
    , .wait_cond = FI_CQ_COND_NONE
    , .wait_set = NULL
    };
    struct fi_eq_attr eq_attr = {
      .size = 128
    , .flags = 0
    , .wait_obj = FI_WAIT_UNSPEC
    , .signaling_vector = 0     /* don't care */
    , .wait_set = NULL          /* don't care */
    };
    struct fid_av *av;
    get_state_t *gst = &global_state.u.get;
    rcvr_t *r = &gst->rcvr;
    sink_t *s = &gst->sink;
    session_t sess;
    struct fi_cq_msg_entry completion;
    ssize_t ncompleted;
    worker_t *w;
    int rc;

    rc = fi_av_open(global_state.domain, &av_attr, &av, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_av_open");

    rcvr_init(r, av);
    sink_init(s);

    if (!session_init(&sess, &r->cxn, &s->terminal))
        errx(EXIT_FAILURE, "%s: failed to initialize session", __func__);

    if ((rc = fi_endpoint(global_state.domain, global_state.info,
                          &gst->listen_ep, NULL)) != 0)
        bailout_for_ofi_ret(rc, "fi_endpoint");

    if ((rc = fi_eq_open(global_state.fabric, &eq_attr, &gst->listen_eq,
                         NULL)) != 0)
        bailout_for_ofi_ret(rc, "fi_eq_open (listen)");

    if ((rc = fi_cq_open(global_state.domain, &cq_attr, &gst->listen_cq,
                         NULL)) != 0)
        bailout_for_ofi_ret(rc, "fi_cq_open");

    if ((rc = fi_ep_bind(gst->listen_ep, &gst->listen_cq->fid,
        FI_SELECTIVE_COMPLETION | FI_RECV | FI_TRANSMIT)) != 0)
        bailout_for_ofi_ret(rc, "fi_ep_bind (completion queue)");

    if ((rc = fi_eq_open(global_state.fabric, &eq_attr, &r->cxn.eq, NULL)) != 0)
        bailout_for_ofi_ret(rc, "fi_eq_open (active)");

    if ((rc = fi_ep_bind(gst->listen_ep, &gst->listen_eq->fid, 0)) != 0)
        bailout_for_ofi_ret(rc, "fi_ep_bind (event queue)");

    if ((rc = fi_ep_bind(gst->listen_ep, &av->fid, 0)) != 0)
        bailout_for_ofi_ret(rc, "fi_ep_bind (address vector)");

    if ((rc = fi_enable(gst->listen_ep)) != 0)
        bailout_for_ofi_ret(rc, "fi_enable");

    rc = fi_recvmsg(gst->listen_ep, &(struct fi_msg){
          .msg_iov = r->initial.iov
        , .desc = r->initial.desc
        , .iov_count = r->initial.niovs
        , .addr = r->cxn.peer_addr
        , .context = NULL
        , .data = 0
        }, FI_COMPLETION);

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_recvmsg");

    /* Await initial message. */
    do {
        ncompleted = fi_cq_sread(gst->listen_cq, &completion, 1, NULL, -1);
        if (ncompleted == -FI_EINTR)
            hlog_fast(signal, "%s: fi_cq_sread interrupted", __func__);
    } while (ncompleted == -FI_EAGAIN ||
             (ncompleted == -FI_EINTR && !cancelled));

    if (cancelled)
        errx(EXIT_FAILURE, "caught a signal, exiting.");

    if (ncompleted < 0)
        bailout_for_ofi_ret(ncompleted, "fi_cq_sread");

    if (ncompleted != 1) {
        errx(EXIT_FAILURE,
            "%s: expected 1 completion, read %zd", __func__, ncompleted);
    }

    if ((completion.flags & desired_rx_flags) != desired_rx_flags) {
        errx(EXIT_FAILURE,
            "%s: expected flags %" PRIu64 ", received flags %" PRIu64,
            __func__, desired_rx_flags, completion.flags & desired_rx_flags);
    }

    if (completion.len != sizeof(r->initial.msg)) {
        errx(EXIT_FAILURE,
            "initially received %zu bytes, expected %zu\n", completion.len,
            sizeof(r->initial.msg));
    }

    if (r->initial.msg.nsources != 1 || r->initial.msg.id != 0) {
        errx(EXIT_FAILURE,
            "received nsources %" PRIu32 ", id %" PRIu32 ", expected 1, 0\n",
            r->initial.msg.nsources, r->initial.msg.id);
    }

    rc = fi_av_insert(r->cxn.av, r->initial.msg.addr, 1, &r->cxn.peer_addr,
        0, NULL);

    if (rc < 0) {
        bailout_for_ofi_ret(rc, "fi_av_insert initial.msg.addr %p",
            r->initial.msg.addr);
    }

    struct fi_info *ep_info, *hints = fi_dupinfo(global_state.info);

    hints->dest_addr = r->initial.msg.addr;
    hints->dest_addrlen = r->initial.msg.addrlen;
    hints->src_addr = NULL;
    hints->src_addrlen = 0;

    rc = fi_getinfo(FI_VERSION(1, 13), NULL, NULL, 0, hints, &ep_info);

    if ((rc = fi_endpoint(global_state.domain, ep_info, &r->cxn.ep, NULL)) < 0)
        bailout_for_ofi_ret(rc, "fi_endpoint");

    hints->dest_addr = NULL;    // fi_freeinfo wants to free(3) dest_addr
    hints->dest_addrlen = 0;
    fi_freeinfo(hints);

    fi_freeinfo(ep_info);

    if ((rc = fi_ep_bind(r->cxn.ep, &r->cxn.eq->fid, 0)) < 0)
        bailout_for_ofi_ret(rc, "fi_ep_bind");

    if ((rc = fi_cq_open(global_state.domain, &cq_attr, &r->cxn.cq, NULL)) != 0)
        bailout_for_ofi_ret(rc, "fi_cq_open");

    if ((rc = fi_ep_bind(r->cxn.ep, &r->cxn.cq->fid,
        FI_SELECTIVE_COMPLETION | FI_RECV | FI_TRANSMIT)) != 0)
        bailout_for_ofi_ret(rc, "fi_ep_bind");

    if ((rc = fi_ep_bind(r->cxn.ep, &av->fid, 0)) != 0)
        bailout_for_ofi_ret(rc, "fi_ep_bind (address vector)");

    if ((rc = fi_enable(r->cxn.ep)) != 0)
        bailout_for_ofi_ret(rc, "fi_enable");

    size_t addrlen = sizeof(r->ack.msg.addr);

    if ((rc = fi_getname(&r->cxn.ep->fid, r->ack.msg.addr, &addrlen)) != 0)
        bailout_for_ofi_ret(rc, "fi_getname");

    r->ack.msg.addrlen = (uint32_t)addrlen;

    while ((rc = fi_sendmsg(r->cxn.ep, &(struct fi_msg){
          .msg_iov = r->ack.iov
        , .desc = r->ack.desc
        , .iov_count = r->ack.niovs
        , .addr = r->cxn.peer_addr
        , .context = NULL
        , .data = 0
        }, 0)) == -FI_EAGAIN) {
        ncompleted = fi_cq_read(r->cxn.cq, &completion, 1);

        if (ncompleted == -FI_EAGAIN)
            continue;

        if (ncompleted < 0)
            bailout_for_ofi_ret(ncompleted, "fi_cq_sread");

        if (ncompleted != 1) {
            errx(EXIT_FAILURE,
                "%s: expected 1 completion, read %zd", __func__, ncompleted);
        }

        errx(EXIT_FAILURE,
            "%s: expected flags %" PRIu64 ", received flags %" PRIu64,
            __func__, desired_rx_flags, completion.flags & desired_rx_flags);
    }

    if (rc < 0)
        bailout_for_ofi_ret(rc, "fi_sendmsg");

    if ((w = workers_assign_session(&sess)) == NULL) {
        errx(EXIT_FAILURE, "%s: could not assign a new receiver to a worker",
            __func__);
    }

    return workers_join_all();
}

static int
put(void)
{
    struct fi_av_attr av_attr = {
      .type = FI_AV_UNSPEC
    , .rx_ctx_bits = 0
    , .count = 0
    , .ep_per_node = 0
    , .name = NULL
    , .map_addr = NULL
    , .flags = 0
    };
    struct fi_cq_attr cq_attr = {
      .size = 128
    , .flags = 0
    , .format = FI_CQ_FORMAT_MSG
    , .wait_obj = FI_WAIT_UNSPEC
    , .signaling_vector = 0
    , .wait_cond = FI_CQ_COND_NONE
    , .wait_set = NULL
    };
    struct fi_eq_attr eq_attr = {
      .size = 128
    , .flags = 0
    , .wait_obj = FI_WAIT_UNSPEC
    , .signaling_vector = 0     /* don't care */
    , .wait_set = NULL          /* don't care */
    };
    session_t sess;
    put_state_t *pst = &global_state.u.put;
    xmtr_t *x = &pst->xmtr;
    source_t *s = &pst->source;
    struct fid_av *av;
    worker_t *w;
    int rc;

    rc = fi_av_open(global_state.domain, &av_attr, &av, NULL);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_av_open");

    xmtr_init(x, av);
    source_init(s);

    if (!session_init(&sess, &x->cxn, &s->terminal))
        errx(EXIT_FAILURE, "%s: failed to initialize session", __func__);

    if ((rc = fi_endpoint(global_state.domain, global_state.info, &x->cxn.ep,
                          NULL)) != 0)
        bailout_for_ofi_ret(rc, "fi_endpoint");

    if ((rc = fi_cq_open(global_state.domain, &cq_attr, &x->cxn.cq, NULL)) != 0)
        bailout_for_ofi_ret(rc, "fi_cq_open");

    if ((rc = fi_eq_open(global_state.fabric, &eq_attr, &x->cxn.eq, NULL)) != 0)
        bailout_for_ofi_ret(rc, "fi_eq_open");

    if ((rc = fi_ep_bind(x->cxn.ep, &x->cxn.eq->fid, 0)) != 0)
        bailout_for_ofi_ret(rc, "fi_ep_bind");

    if ((rc = fi_ep_bind(x->cxn.ep, &x->cxn.cq->fid,
        FI_SELECTIVE_COMPLETION | FI_RECV | FI_TRANSMIT)) != 0)
        bailout_for_ofi_ret(rc, "fi_ep_bind");

    if ((rc = fi_ep_bind(x->cxn.ep, &av->fid, 0)) != 0)
        bailout_for_ofi_ret(rc, "fi_ep_bind (address vector)");

    if ((rc = fi_enable(x->cxn.ep)) != 0)
        bailout_for_ofi_ret(rc, "fi_enable");

    rc = fi_av_insert(av, global_state.info->dest_addr, 1, &x->cxn.peer_addr, 0,
        NULL);

    if (rc < 0) {
        bailout_for_ofi_ret(rc, "fi_av_insert dest_addr %p",
            global_state.info->dest_addr);
    }

    /* Setup initial message. */
    memset(&x->initial.msg, 0, sizeof(x->initial.msg));
    x->initial.msg.nsources = 1;
    x->initial.msg.id = 0;

    x->initial.desc = fi_mr_desc(x->initial.mr);

    size_t addrlen = sizeof(x->initial.msg.addr);

    if ((rc = fi_getname(&x->cxn.ep->fid, x->initial.msg.addr, &addrlen)) != 0)
        bailout_for_ofi_ret(rc, "fi_getname");

    x->initial.msg.addrlen = (uint32_t)addrlen;

    if ((w = workers_assign_session(&sess)) == NULL) {
        errx(EXIT_FAILURE, "%s: could not assign a new transmitter to a worker",
            __func__);
    }

    return workers_join_all();
}

static int
count_info(const struct fi_info *first)
{
    int count;
    const struct fi_info *info;

    for (info = first, count = 1; (info = info->next) != NULL; count++)
        ;

    return count;
}

static const char *
personality_to_name(personality_t p)
{
    if (p == get)
        return "fget";
    else if (p == put)
        return "fput";
    else
        return "unknown";
}

static void
usage(personality_t personality, const char *progname)
{
    if (personality == put)
        fprintf(stderr, "usage: %s [-r] [-g] <address>\n", progname);
    else
        fprintf(stderr, "usage: %s [-b <address>] [-r]\n", progname);

    exit(EXIT_FAILURE);
}

static void
handler(int signum, siginfo_t *info, void *ucontext)
{
    cancelled = 1;
}

int
main(int argc, char **argv)
{
    sigset_t oldset, blockset;
    struct fi_info *hints;
    personality_t personality;
    const char *addr = NULL;
    char *progname, *tmp;
    int ecode, i, opt, rc;

    if ((tmp = strdup(argv[0])) == NULL)
        err(EXIT_FAILURE, "%s: strdup", __func__);

    progname = basename(tmp);

    if (strcmp(progname, "fget") == 0) {
        personality = get;
    } else if (strcmp(progname, "fput") == 0) {
        personality = put;
    } else {
        errx(EXIT_FAILURE, "program personality '%s' is not implemented",
           progname);
    }

    const char *optstring = (personality == get) ? "b:r" : "gr";

    while ((opt = getopt(argc, argv, optstring)) != -1) {
        switch (opt) {
        case 'g':
            global_state.contiguous = true;
            break;
        case 'r':
            global_state.reregister = true;
            break;
        case 'b':
            addr = optarg;
            break;
        default:
            usage(personality, progname);
        }
    }

    argc -= optind;
    argv += optind;

    if (personality == put) {
        if (argc != 1)
            usage(personality, progname);
        addr = argv[0];
    } else if (argc != 0)
        usage(personality, progname);

    workers_initialize();

    keysource_init(&global_state.keys);

    hlog_fast(params,
        "%ld POSIX I/O vector items maximum", sysconf(_SC_IOV_MAX));

    if ((hints = fi_allocinfo()) == NULL)
        errx(EXIT_FAILURE, "%s: fi_allocinfo", __func__);

    hints->ep_attr->type = FI_EP_RDM;
    hints->caps = FI_MSG | FI_RMA | FI_REMOTE_WRITE | FI_WRITE;
    hints->mode = FI_CONTEXT;
    hints->domain_attr->mr_mode = FI_MR_PROV_KEY;

    rc = fi_getinfo(FI_VERSION(1, 13), addr,
        fget_fput_service_name, (personality == get) ? FI_SOURCE : 0, hints,
        &global_state.info);

    fi_freeinfo(hints);

    switch (-rc) {
    case FI_ENODATA:
        hlog_fast(err, "capabilities not available?");
        break;
    case FI_ENOSYS:
        hlog_fast(err, "available libfabric version < 1.13?");
        break;
    default:
        break;
    }

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_getinfo");

    hlog_fast(params, "%d infos found", count_info(global_state.info));

    if ((global_state.info->mode & FI_CONTEXT) != 0) {
        errx(EXIT_FAILURE,
           "contexts should embed fi_context, but I don't do that, yet.");
    }

    rc = fi_fabric(global_state.info->fabric_attr, &global_state.fabric,
        NULL /* app context */);

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_fabric");

    rc = fi_domain(global_state.fabric, global_state.info, &global_state.domain,
        NULL);

    hlog_fast(params, "provider %s, memory-registration I/O vector limit %zu",
        global_state.info->fabric_attr->prov_name,
        global_state.info->domain_attr->mr_iov_limit);

    hlog_fast(params,
        "provider %s %s application-requested memory-registration keys",
        global_state.info->fabric_attr->prov_name,
        ((global_state.info->domain_attr->mr_mode & FI_MR_PROV_KEY) != 0)
            ? "does not support"
            : "supports");

    if ((global_state.info->domain_attr->mr_mode & FI_MR_VIRT_ADDR) != 0) {
        hlog_fast(params,
            "provider %s RDMA uses virtual addresses instead of offsets, "
            "quitting.", global_state.info->fabric_attr->prov_name);
        exit(EXIT_FAILURE);
    }

    hlog_fast(params, "Rx/Tx I/O vector limits %zu/%zu",
        global_state.info->rx_attr->iov_limit,
        global_state.info->tx_attr->iov_limit);

    hlog_fast(params, "RMA I/O vector limit %zu",
        global_state.info->tx_attr->rma_iov_limit);

    /* Always use 1 because there are problems with using mr_maxsegs > 1,
     * i.e., global_state.info->domain_attr->mr_iov_limit.
     */
    global_state.mr_maxsegs = 1;
    global_state.rx_maxsegs = 1;
    global_state.tx_maxsegs = 1;
    global_state.rma_maxsegs = global_state.contiguous
        ? 1
        : global_state.info->tx_attr->rma_iov_limit;

#if 0
    hlog_fast(params, "maximum endpoint message size (RMA limit) %zu",
        global_state.info->ep_attr->max_msg_size);
#endif

    if (rc != 0)
        bailout_for_ofi_ret(rc, "fi_domain");

    hlog_fast(params, "starting personality '%s'",
        personality_to_name(personality));

    if (sigemptyset(&blockset) == -1)
        err(EXIT_FAILURE, "%s.%d: sigemptyset", __func__, __LINE__);

    for (i = 0; i < arraycount(siglist); i++) {
        if (sigaddset(&blockset, siglist[i].signum) == -1)
            err(EXIT_FAILURE, "%s.%d: sigaddset", __func__, __LINE__);
    }

    if ((rc = pthread_sigmask(SIG_BLOCK, &blockset, &oldset)) != 0) {
        errx(EXIT_FAILURE, "%s.%d: pthread_sigmask: %s", __func__, __LINE__,
            strerror(rc));
    }

    struct sigaction action = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO};

    if (sigemptyset(&action.sa_mask) == -1)
        err(EXIT_FAILURE, "%s.%d: sigaddset", __func__, __LINE__);

    for (i = 0; i < arraycount(siglist); i++) {
        if (sigaction(siglist[i].signum, &action,
                      &siglist[i].saved_action) == -1)
            err(EXIT_FAILURE, "%s.%d: sigaddset", __func__, __LINE__);
    }

    if ((rc = pthread_sigmask(SIG_UNBLOCK, &blockset, NULL)) != 0) {
        errx(EXIT_FAILURE, "%s.%d: pthread_sigmask: %s", __func__, __LINE__,
            strerror(rc));
    }

    ecode = (*personality)();

    if ((rc = pthread_sigmask(SIG_SETMASK, &oldset, NULL)) != 0) {
        errx(EXIT_FAILURE, "%s.%d: pthread_sigmask: %s", __func__, __LINE__,
            strerror(rc));
    }

    return ecode;
}