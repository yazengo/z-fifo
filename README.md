# zfifo

参考 Zephyr `spsc_lockfree` 的单生产者/单消费者（1P1C）无锁环形 FIFO，通用元素大小，面向嵌入式。提供两个版本，接口一一对应：

| 版本 | 文件 / 前缀 | 容量 | 下标计算 | M33 代码量 |
|---|---|---|---|---|
| 任意容量 | `zfifo.h/.c`，`zfifo_*` | 1 ~ 2^31-1 | 比较 + 减法 | 832 B |
| 2 的幂 | `zfifo_2n.h/.c`，`zfifo_2n_*` | 2 ~ 2^31，2 的幂 | 位与掩码 | 632 B |

一般用 `zfifo`；容量本来就是 2 的幂、又在热路径上时用 `zfifo_2n`。两者都不浪费槽位，满时恰好存 `cap` 个元素。

## 设计要点

- `in` / `out` 分别只由生产者、消费者写入，`in == out` 为空。
  - `zfifo_2n`：32 位自由递增计数器，自然回绕，`in - out` 即已用数量。
  - `zfifo`：计数器在 `[0, 2·cap)` 内回绕，相差 `cap` 为满；`advance()` 写法避免大容量时 32 位溢出。
- 同步只靠 acquire/release：生产者 `load_acquire(out)` → 写数据 → `store_release(in)`；消费者对称。
  原子访问宏在 `zfifo_port.h`：GCC/Clang 用 `__atomic`（Cortex-M33 生成 `LDA/STL`，M3/M4 生成 `DMB`），Keil ARMCC5 用 `__dmb`，其他编译器自定义 `ZFIFO_LOAD_ACQ/ZFIFO_STORE_REL`。
- 无锁、不关中断，适合 ISR ↔ 线程、线程 ↔ 线程；多生产者或多消费者需自行加锁。
- `used/space` 由生产者或消费者调用时精确，第三方上下文调用只是近似值。

## API（以 `zfifo` 为例，`zfifo_2n` 同名加 `_2n`）

| 侧 | 接口 |
|---|---|
| 初始化 | `ZFIFO_DEFINE(name, type, cap)`、`zfifo_init()`、`zfifo_reset()`（两侧空闲时） |
| 查询 | `zfifo_used/space/capacity/is_empty/is_full` |
| 生产者 | `zfifo_put`、`zfifo_write`（批量，返回实际写入数）、`zfifo_write_acquire/commit`（零拷贝） |
| 消费者 | `zfifo_get`、`zfifo_peek`、`zfifo_read`、`zfifo_drop`、`zfifo_flush`、`zfifo_read_acquire/release`（零拷贝） |

```c
ZFIFO_DEFINE(uart_rx, uint8_t, 300);

void UART_IRQHandler(void) { uint8_t c = UART->DR; zfifo_put(&uart_rx, &c); }

void rx_task(void)
{
    uint8_t *p;
    uint32_t n = zfifo_read_acquire(&uart_rx, (void **)&p);
    parse(p, n);
    zfifo_read_release(&uart_rx, n);
}
```

## 测试

```sh
make test        # 两个版本的单元测试 + 双线程压力测试
make test-tsan   # ThreadSanitizer 下同样测试
```
