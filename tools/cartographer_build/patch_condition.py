p = '/root/cartographer/cartographer/common/internal/testing/thread_pool_for_testing.cc'
c = open(p).read()
c = c.replace(
    'const auto predicate = [this]()\n                             EXCLUSIVE_LOCKS_REQUIRED(mutex_) { return idle_; };',
    'const auto predicate = [](ThreadPoolForTesting* self) { return self->idle_; };')
c = c.replace(
    'const auto predicate = [this]() EXCLUSIVE_LOCKS_REQUIRED(mutex_) {\n    return !task_queue_.empty() || !running_;\n  };',
    'const auto predicate = [](ThreadPoolForTesting* self) {\n    return !self->task_queue_.empty() || !self->running_;\n  };')
c = c.replace('absl::Condition(&predicate)', 'absl::Condition(+predicate, this)')
open(p, 'w').write(c)
print('patched', c.count('absl::Condition(+predicate, this)'))
