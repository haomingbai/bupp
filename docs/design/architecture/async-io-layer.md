# Layer 2: `bnio::async_io` — Platform-Neutral Vocabulary Types

Namespace `bnio::async_io`. Headers in `include/bnio/async_io/`.

This layer defines **vocabulary types only**: non-owning views and value types.
It intentionally contains **zero RAII owners**.

### Non-Owning Views

#### `buffer_view`

```cpp
struct buffer_view {
    void* data = nullptr;
    std::size_t size = 0;
};
```

Plain aggregate. Copy copies pointer and size; the data is not owned.

#### `descriptor_view`

Non-owning wrapper around a native file descriptor (`int`). Destruction does
**not** close the descriptor. I/O on this view is **streaming**: every
operation transfers at the kernel file position and advances it.

#### `random_access_file`

Non-owning wrapper around a native file descriptor, constructible explicitly
from an `int` or a `descriptor_view`, with the same `native_handle()` /
`valid()` interface. I/O on this view is **positioned**: every operation takes
an explicit offset (no default) and leaves the kernel file position untouched.
Like `descriptor_view`, it owns nothing and destruction does not close the
descriptor.

#### Socket View Family

Socket-kind-specific non-owning views built on `socket_view`:

| Type | Native Kind | Operations |
|------|-------------|------------|
| `socket_view` | Unspecified | Descriptor access only |
| `stream_socket_view` | `SOCK_STREAM` | `bind()`, `listen()`, `connect()`, `shutdown()`, socket options |
| `datagram_socket_view` | `SOCK_DGRAM` | `bind()`, `connect()`, endpoint queries; transfer is async-only |

Typed views can be constructed from `socket_view` and all hold the same fd
value. Listening is a lifecycle state of a stream socket, not a separate
socket kind. Stream and datagram operations remain distinct so stream partial
I/O rules cannot be applied to a datagram.

### Value Types

| Type | Description |
|------|-------------|
| `async_io::ip::address` | IPv4 or IPv6 address |
| `async_io::ip::endpoint` | IP address + port number |
| `async_io::ip::tcp` | TCP protocol tag (v4/v6/any) |
| `async_io::ip::udp` | UDP protocol tag |

These are copyable, self-contained value types.

### `linux_native::io_uring_context` — Platform Event Loop

Within `bnio::async_io::linux_native`, `io_uring_context` owns a `base::ring`
and provides a single-threaded event loop. Under the **one-thread-one-uring**
model, each run-loop thread owns its own `io_uring_context` (allocated from
`io_context`'s native worker pool). All workers in one high-level context share
an `io_uring_task_queue_state`. That state contains separate MPSC CPU and I/O
queues, the number of workers currently published as awake, the total number
of workers inside `io_context::run()` (`running_workers`, maintained by
`io_context` itself), the suspend worker-state
list (used by directed wakeup), and the stopping
state (`life_state`) of the whole worker group. It deliberately has no I/O count, batch
threshold, explicit-drain flag, or timer. A standalone
`io_uring_context` must likewise be given an externally owned task queue state
before `run()`.

Rings using `IORING_SETUP_SINGLE_ISSUER` are created with
`IORING_SETUP_R_DISABLED`. The owning run-loop thread enables its ring before
the first submission, making that thread the kernel-designated issuer even
when the context object was constructed elsewhere.

```cpp
class io_uring_context {
public:
    io_uring_context() noexcept;
    explicit io_uring_context(const io_uring_context_options& opts) noexcept;
    ~io_uring_context() noexcept;

    // non-copyable, non-movable (owns a ring and run-loop state)

    int queue_init(const io_uring_context_options& opts) noexcept;
    void queue_exit() noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] unsigned kernel_features() const noexcept;
    void set_global_state(io_uring_task_queue_state& state) noexcept;

    // Sender factories
    [[nodiscard]] auto async_poll(descriptor_view descriptor,
                                   unsigned poll_mask);
    [[nodiscard]] auto async_resolve(dns_query query,
                                      dns_result_view result);
    [[nodiscard]] auto async_resolve(std::string_view host,
                                      std::string_view service,
                                      dns_result_view result);

    void notify_one_waiter() noexcept;
    [[nodiscard]] bool is_waiting() const noexcept;

    int post(io_uring_operation_base& op) noexcept;
    void publish_io(io_uring_io_operation_base& op) noexcept;
    void run() noexcept;
    int stop() noexcept;
    [[nodiscard]] bool is_in_context() const noexcept;
};
```

Each context stores one normalized `io_uring_context_options` value instead of
copying its fields into separate members. Its single run thread is the only
owner of SQ preparation, submission, and CQ collection, so the Linux async-io
layer contains no ring mutex. Low-level senders publish an
`io_uring_io_operation_base` to one of the two I/O queues. There is no
public raw prepare, submit, or batch-submit interface. Only the run loop takes
I/O operations, fills SQEs, and calls the private ring-submission helper.

Every worker owns a **local I/O queue** next to its local CPU queue
(`io_uring_local_task_queue_state::io_head`). `publish_io()` takes the same
worker-local fast path as `post()`: an operation published from a callback
running on this context's run loop goes to the worker's own queue, and every
other publication goes to the shared MPSC I/O queue and wakes a worker.
`consume_io_tasks()` pops the local queue first and falls back to the shared
queue only when the local one is empty. The local queue needs no lock, no
atomic, and no wakeup: the only thread that can push to it is the thread that
drains it — the publisher is the consumer — and no other worker ever touches
it (there is no I/O stealing). The queue links operations through the `next`
field inherited from `io_uring_operation_base`, the same intrusive link the
local CPU queue and the shared I/O queue use, because on io_uring
`io_next`/`io_prev` belong exclusively to the inflight doubly-linked list. The
kqueue backend links its I/O queues through `io_next` instead: an operation is
either queued or inflight, never both, so there `io_next` is free to double as
the queue link while `io_next`/`io_prev` together carry the inflight list.

SQ pressure is transient and is never retried inline. `prepare_io()` returns
`-EAGAIN` when `get_sqe()` finds no free slot; `consume_io_tasks()` then
submits the SQEs it already prepared and leaves the operations it could not
prepare in the run-loop-owned retry slot (`pending_io_retry_`), kept outside
every queue. The next run-loop pass retries that list in the original order
before popping any queue — no reversal, no re-push. On stop,
`abort_inflight_io()` drains the retry slot together with the two I/O queues.
`arm_wake_poll()` follows the same rule: a full SQ makes it return `-EAGAIN`
immediately, and the run loop re-enters the ready-tasks phase, which re-arms
the poll on a later pass.

A failed submit splits the batch the same way. `submit_and_track_prepared()`
registers every prepared operation in the inflight list before the enter call,
so its participants are exactly the operations that can be untracked and
failed with the submit's errno through `fail_io_list()`. The remainder is
designed for retryable errors, so `consume_io_tasks()` re-queues it
unchanged whenever `is_transient_submit_error()` classifies the errno as one
the next pass can plausibly clear (`-EINTR`, `-EAGAIN`, `-EBUSY`, `-ENOMEM`):
none of these says anything about the ring's health, and the remainder holds
no SQE and no inflight registration that a re-queue would have to undo. For
any other errno the remainder fails with the same errno as the prepared head:
those errnos (`-EBADFD`, `-EINVAL` and friends) mean the ring rejects every
submission, so an unconditional re-queue would busy-spin the run loop through
the retry slot forever and never let the fatal-error routing run. The split
keeps one operation's outcome independent of where it happened to sit in the
batch when the SQ filled.

Each ready-task pass first drains CQEs to keep the completion ring from
overflowing, then checks local and shared CPU work before consuming I/O — the
worker's own local I/O queue first, the shared I/O queue only when the local
one is empty. Before eventfd wait it publishes the
local waiting flag, decrements the shared awake count, and repeats the
CQE/CPU/I/O checks. A producer publishing after that transition wakes exactly
one sleeping worker through its per-worker wake channel (`wake_one_sleeping`),
falling back to the shared broadcast channel only when nobody is suspended.
This supplies low-load progress while busy workloads form batches naturally.

Wake channels come from two places: a per-worker channel opened by
`queue_init()` (`local_state()->wake_channel_`) for directed wakeups, and the
shared broadcast channel owned by the task queue state
(`io_uring_task_queue_state::wake_channel_`), used for stop and native
cross-thread publication. The `io_uring_context_options::event_fd` field is
retained for API compatibility but is not consulted by the implementation.

#### Internal submission contract

`post()` and `publish_io()` are internal submission APIs. They perform no
lifecycle gating at this layer: neither consults the shared `life_state`, and
both always enqueue. Gating happens one layer up, in the `io_context` queue
(`bnio::io_context::publish_io` / `publish_cpu`): it serializes against
`begin_stop()` / `~io_context()` with `submit_lock`, checks `life_state`, and
refuses work once stopping begins — the caller then completes the operation
inline, so a submission through that layer never strands.

The async_io layer omits the gate because these are internal interfaces: it
assumes a correctly driven lifecycle. Anyone calling `post()` or
`publish_io()` directly must guarantee the context keeps running until every
submitted operation reaches a terminal receiver call (a `set_value` —
success, error, or `operation_canceled` — or a `set_stopped` on token
cancellation).

This layer also does not observe `io_context` stop. Token cancellation is
honored everywhere (a stopped branch that sees a cancelled receiver token
delivers `set_stopped()`), but a posted CPU task drained during a stop
delivers `set_value({})`, and `async_resolve()` runs and delivers its real
result. This is the intentional layering difference from the `io_context`
schedule sender, whose single observation point arbitrates the receiver
token first and otherwise reports `set_value(operation_canceled)` when the
context stopped. Only the high-level layer, which owns `life_state`, can
implement that not-yet-executed-work rule.

#### Error routing and delivery guarantees

Run-loop failures never strand operations. The phase machine routes fatal
errors — a failed wake-poll re-arm, a fatal `io_uring_enter` error, or a
failed `enter_run()` setup — through `finish_drain` (or, in `enter_run()`,
directly into `finish()`): drain ready CQEs and CPU tasks, abort remaining
inflight and shared-queued I/O, and deliver every completion before the
context reaches `finished`. A fatal ring or wake-channel error therefore
cannot exit the loop with receivers left silent.

Delivery through the stop path is two-stage. `complete_submit_error`
delivers `set_value(ec, ...)` directly. `complete_submit_stopped()` only
marks the completion as stopped (the stop channel); the final signal is
decided by the token arbitration in each operation's `execute()` stopped
branch: if the receiver's stop token is cancelled, the operation delivers
`set_stopped()`; otherwise it delivers
`set_value(operation_canceled, ...)` — the not-yet-executed queued-work
rule of `io_context::stop()`. The `value`/`value_with_ec` branches of
`execute()` never consult the token, so already-completed results and
kernel-level `ECANCELED` are delivered unchanged.

The stopped branch of `execute()` carries no result: the request never ran,
so it delivers `-1` as the native result and `0` as the flags instead of
synthesizing a value. Byte-count models clamp with `std::max(0, result)`, so
a cancelled read or write still observes `0` bytes, and `connect` ignores the
result entirely. A descriptor-yielding model has no clamp available —
`kqueue_accept_request` and `io_uring_accept_request` forward `result`
verbatim, `0` is a legal descriptor, and `tcp::socket` owns whatever it is
handed, so a synthesized `0` wrapped the process's stdin in an owning socket
(`O_NONBLOCK` in the constructor, `close()` in the destructor). `-1` is the
only value that can mean "no descriptor", and for the same reason it is the
right value for the abort channel generally: all three sites —
`native_io_operation` on both platforms and the kqueue `io_request` — deliver
it.

Transient `-EAGAIN` from a wake-poll re-arm is not fatal. It means the SQ is
under pressure (typically SQPOLL, where only the kernel poll thread frees SQ
slots) and the poll is not armed: the run loop returns to the ready-tasks
phase instead of blocking, and a later pass re-arms once the kernel consumes
the queued SQEs. The collect path applies the same policy: a re-arm failure
while collecting an eventfd CQE is ignored and re-handled by the single
policy point in `wait_for_io_work()`, so transient pressure never
half-closes the context.

A teardown guard at the top of `consume_io_tasks()` protects the
cancellation contract once the context is stopping or closing: I/O
operations consumed from the SQ-full retry slot or either I/O queue are
then routed straight through `drain_io_list_complete_stopped()` instead of
being prepared and submitted — with the ring going away, a submit failure
such as `EBADFD` would surface as a generic error where the contract
requires the stop channel, and the guard also implements the
not-yet-executed queued-work rule. The guard gates only the
stopping/closing state; the normal run path is unchanged. kqueue
`consume_io_tasks()` carries the same teardown guard
(`kqueue_context::consume_io_tasks()`): an `EV_ADD` during teardown would
arm a new event filter and let an operation published while `finish()`
drains its queues run for real, so on kqueue too the drained operations
are routed through the stop channel instead of being registered. See
[`lifecycle.md`](../lifecycle.md) for the full teardown rules.

`queue_exit()` delivers as well: it marks the context finishing and runs the
same abort-and-deliver path as `finish()` before closing the ring, so a
forced/abnormal close (e.g. destruction without `run()`) marks every
inflight and queued operation through the stop channel instead of
discarding it, and each operation's `execute()` arbitration then delivers
`set_stopped()` (token cancelled) or `set_value(operation_canceled, ...)`.

### `io_uring_context_options`

```cpp
struct io_uring_context_options {
    unsigned entries = 256;
    unsigned setup_flags =
        IORING_SETUP_COOP_TASKRUN |
        IORING_SETUP_SINGLE_ISSUER |
        IORING_SETUP_R_DISABLED;
    unsigned cqe_batch_window = 64;
    unsigned wait_spin_count = 4;
    unsigned cqe_inline_completion_threshold = 64;
    unsigned local_queue_threshold = 0;
    bool enable_sqpoll = false;
    unsigned sqpoll_thread_cpu = 0;
    unsigned sqpoll_idle_ms = 1000;
    int event_fd = -1;
};
```

`set_global_state()` is a non-thread-safe setter that must receive externally
owned shared state before work is published or `run()` is called. Its owner
must keep it alive until the context stops. `io_context` owns one such state
and calls the setter for every native worker. `io_uring_context` never owns or
manufactures a fallback global state.

CPU operations derive from `io_uring_operation_base`. I/O operations add the
prepare contract and reuse the base class's intrusive link:

```cpp
class io_uring_operation_base {
public:
    io_uring_operation_base* next = nullptr;
    int result = 0;
    unsigned flags = 0;

    // non-copyable, non-movable (intrusive list node)

    virtual ~io_uring_operation_base() = default;
    virtual void execute() noexcept = 0;
};

class io_uring_io_operation_base : public io_uring_operation_base {
public:
    virtual void prepare(base::submission_queue_entry& sqe) noexcept = 0;
    virtual void complete_submit_error(int result) noexcept = 0;
    virtual void complete_submit_stopped() noexcept = 0;
};

struct io_uring_task_queue_state {
    std::atomic<io_uring_operation_base*> cpu_head;
    std::atomic<io_uring_io_operation_base*> io_head;
    std::atomic<std::size_t> awake_workers;
    std::atomic<std::size_t> running_workers;   // total workers inside io_context::run() (maintained by io_context)
    io_uring_worker_state_list workers;     // sleeping workers for directed wake
    std::atomic<int> life_state{0};  // 0 = running, 1 = stopping
    void* timeout_heap = nullptr;
    try_fetch_timeout_fn try_fetch_timeout_operations = nullptr;
    bnio::base::wake_channel wake_channel_;     // shared broadcast (stop + native notify_one_waiter)
    std::mutex submit_lock;                     // publish/stop serialization
};
```

`life_state` belongs to the worker group. Once `io_context::stop()` sets it
to 1 (stopping) via CAS, every worker treats the run loop as finishing,
including a worker racing with late registration. Workers check
`closing_requested()` (life_state != 0) in `should_finish()`.

Ready CQEs are collected at the beginning of every ready-task pass. The run
loop then drains its local CPU list and the shared CPU queue before consuming
I/O — its own local I/O queue first, the shared I/O queue only when the local
one is empty.

Parking is a two-stage publication. The worker first marks its local state as
waiting, then decrements `awake_workers`. It rechecks CQEs and CPU work and
again takes all I/O before reading eventfd. If work appeared, it
increments `awake_workers`, reopens locally, and resumes. A producer that sees
a waiting worker writes that worker's eventfd, closing the lost-wakeup window
without a timer. Each worker owns a **per-worker** wake channel for directed
wakeup (one sleeping worker is enough for a single publication), plus the
shared channel used for broadcast (stop) and as a fallback when nobody is
suspended. CPU-task fetching (`fetch_cpu_task()` → local → shared) and
the suspend worker-state list are shared with the kqueue backend;
see [`worker-scheduling.md`](worker-scheduling.md).

### `bsd_native::kqueue_context` — Passive Readiness Event Loop

The BSD backend uses the same CPU/I/O publication split. A
`kqueue_task_queue_state` owns separate MPSC heads, `awake_workers` /
`running_workers` (the latter maintained by `io_context::run()`), the
suspend worker-state list, and the
worker-group `life_state` flag (std::atomic<int>, 0=running, 1=stopping). Local CPU and I/O queues are
non-atomic in all modes — both are owned exclusively by the worker thread.

```cpp
class kqueue_context {
public:
    int queue_init(const kqueue_context_options& opts = {}) noexcept;
    void queue_exit() noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    void set_global_state(kqueue_task_queue_state* state) noexcept;

    int post(kqueue_operation_base& op) noexcept;
    void publish_io(kqueue_io_operation_base& op) noexcept;
    void notify_one_waiter() noexcept;
    [[nodiscard]] bool is_waiting() const noexcept;

    void run() noexcept;
    int stop() noexcept;
    [[nodiscard]] bool is_in_context() const noexcept;
};
```

Starting readiness-backed work only publishes a
`kqueue_io_operation_base`. The run-loop thread is the sole owner of
`kqueue_helper` preparation, `kevent()` registration, and the active
registration table. The backend consequently exposes no public `prepare()`,
`submit()`, or batch-submit API and has no submission or registration mutex.

CPU completions run before I/O publication is consumed. Immediately before a
blocking `kevent()` call, the worker marks itself waiting, updates the shared
awake count, and rechecks events, CPU work, I/O work, and shutdown. Producers
that observe a waiting worker wake exactly one sleeping worker through its
per-worker wake channel (falling back to the shared broadcast channel); no
active submission timer is involved. Worker sleep/wake and
the suspend worker-state list are shared with the io_uring backend — see
[`worker-scheduling.md`](worker-scheduling.md).

The reactor-specific completion path remains distinct from io_uring: after a
read or write filter fires, the context performs one bounded nonblocking I/O
attempt. `EAGAIN` rearms the one-shot filter; a terminal result is queued as a
CPU completion. Poll operations translate filter readiness to poll masks
without performing an extra data syscall.

---
