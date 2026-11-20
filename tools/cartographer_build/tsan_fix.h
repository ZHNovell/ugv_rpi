#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#ifndef PROTOBUF_TSAN_READ
#define PROTOBUF_TSAN_READ(addr) ((void)0)
#endif
#ifndef PROTOBUF_TSAN_WRITE
#define PROTOBUF_TSAN_WRITE(addr) ((void)0)
#endif
#ifndef PROTOBUF_TSAN_DECLARE_MEMBER
#define PROTOBUF_TSAN_DECLARE_MEMBER
#endif
#ifndef ABSL_ATTRIBUTE_LIFETIME_BOUND
#define ABSL_ATTRIBUTE_LIFETIME_BOUND
#endif
#ifndef ABSL_DCHECK
#define ABSL_DCHECK(expr) ((void)0)
#endif
#ifndef GUARDED_BY
#define GUARDED_BY(x)
#endif
#ifndef LOCKS_EXCLUDED
#define LOCKS_EXCLUDED(x)
#endif
#ifndef EXCLUSIVE_LOCKS_REQUIRED
#define EXCLUSIVE_LOCKS_REQUIRED(x)
#endif
#ifndef SHARED_LOCKS_REQUIRED
#define SHARED_LOCKS_REQUIRED(x)
#endif
#ifndef PT_GUARDED_BY
#define PT_GUARDED_BY(x)
#endif
