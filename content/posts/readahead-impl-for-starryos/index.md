+++
title = "为 StarryOS 实现 readahead"
description = "记录 为 StarryOS 实现 readahead 的开发日志"
date = 2025-12-29
draft = false

[taxonomies]
categories = ["Devlog"]
tags = ["OS"]
+++

# 前瞻

本文记录将 readahead 策略移植到 StarryOS, 加快顺序读取的开发过程。

TODO: 引用论文

## StarryOS & arceos 读取架构

```text
[ User / Syscall ]
      |
      v
[ File (实现了 FileLike) ]
      |
      +--> [ FileBackend ]
             |
             +-- (Cached) --> [ CachedFile ]  <-- Readahead 逻辑在这里
             |                   |
             |                   v
             |            [ Page Cache ]
             |                   | (Cache Miss)
             |                   v
             +-- (Direct) --> [ Location ]
                                 |
                                 v
                          [ DirEntry (Arc<dyn NodeOps>) ]
                                 |
           +---------------------+---------------------+
           |                     |                     |
           v                     v                     v
[ SimpleFile (FileNodeOps) ] [ Device (FileNodeOps) ] [ FatFile (FileNodeOps) ]
           |                     |                     |
   (内存 Vec 操作)         (DeviceOps 转发)      (磁盘驱动操作)
```

## readahead 策略

![strategy](readahead-strategy.png)
# dev log

## 初步实现

将论文描述的预读逻辑添加到 arceos 中 ([commit](https://github.com/uchouT/arceos/commit/1750969021315c9f75a8c5f467d05f9e5a242f58)), 但是经过测试，读取速度反而下降了非常多。通过日志 debug 发现，问题的根源是 async readahead 发起后，没有立刻开始执行，而是要一段时间后 CPU 时钟中断后才开始，但此时应用读取窗口已经消费完了，会遇到本应由 async readahead 填充的 page，造成 cache miss 从而触发预读逻辑；等 async readahead 任务开始执行时，读取的部分是严重重叠的。

解决办法是在 async readahead 提交后，立刻让出 CPU 控制权，让 async readahead 先运行：

```rust,linenos,linenostart=972,name=modules/axfs/src/highlevel/file.rs
                axtask::spawn(move || {
                    // error!(
                    //     "async prefetch launched, pn={} size={} pg_flag={}",
                    //     start_pn, size, pg_readahead_offset
                    // );
                    readahead::async_prefetch(
                        shared,
                        file,
                        in_memory,
                        start_pn,
                        size,
                        pg_readahead_offset,
                    );
                });
                // yield to let async prefetch run earlier
                axtask::yield_now();
```

此时速度终于回到正常水平，但提升效果远没有达到预期。

*TODO: 增加测试数据* 

## 问题探索

观察日志发现，在进入顺序读取时，还是会有 cache miss 发生，而理想情况下这是不应当出现的。

经过思考，原因应该是：如果应用程序读取请求非常快，那么 readahead （producer) 的 cache 生成流水线会立刻被应用程序 (consumer) 追上，从而造成 cache miss，然后又发起 sync readahead，这样就与先前发起的 async readahead 冲突了，造成了大量 CPU 资源浪费。

在这条 [commit](https://github.com/uchouT/arceos/commit/6143c3b9cf720ff166e17fa29e10c67140f8fd67) 所在的时间点中，async readahead 的 cache produce 策略是整块读取，然后统一添加到 LruCache 中, 为了减小 LrchCache 的锁竞争。既然问题是 readahead produce 的速度太慢了，那么很自然的解决办法是让读到的数据尽可能快的添加到 LruCache 中，于是我将 async readahead 的策略从 batch 改为了 streaming: [feat(fs): change async prefetch cache loading from batch to stream](https://github.com/uchouT/arceos/commit/3f967aa2c0f1a731ce022a1097b979571e8cf156).

很可惜，经过测试速度不增反降，不过仔细想想就知道，上面的那个做法相当愚蠢：IO 再快，也不可能比 CPU、内存更快，所以在 consumer 消费非常快的情况下，追上流水线造成 sync readahead 和 async readahead 重叠几乎是必然的，将异步预读改为逐 page 流式处理反倒会引入锁竞争开销。

因此现在的解决方向是：

1. 尽可能加快 async readahead
2. 解决不可避免的 window overlap 冲突

## bounce buffer 加快 io_submit

对于第一个方向，现在的 `io_submit` 策略是逐 pn 发送 `file.read_at` 请求，但是一次 `file.read_at` 其实就是一次 io 请求：

```rs,linenos,linenostart=196,name=modules/axfs/src/highlevel/file/readahead.rs
for &pn in &pages_to_read {
        let mut page = PageCache::new()?;

        if pn == async_pg_pn {
            page.pg_readahead = true;
        }

        if in_memory {
            page.data().fill(0);
        } else {
            file.read_at(page.data(), pn as u64 * PAGE_SIZE as u64)?;
        }
        loaded_pages.push((pn, page));
    }

```

而 readahead 期望的是发送一次性大块 (big chunk) 请求，从而减小硬件的读取开销，比如磁盘寻道。因此现在的实现不符合要求，一次 io submit 实际上仍然有多次 io 请求。现在需要将多次 io 请求合并。

根据 `FileNodeOps` 的 trait，目前的 IO 读取是将 offset 开始的数据填满缓冲区：

```rust
pub trait FileNodeOps: NodeOps + Pollable {
    /// Reads a number of bytes starting from a given offset.
    fn read_at(&self, buf: &mut [u8], offset: u64) -> VfsResult<usize>;
    // ...
}
```

因此如果要利用现有接口实现 big chunk 读取，必须引入一个中间缓冲区 bounce buffer, 将 big chunk 一次性读取到缓冲区中，然后再拷贝到每个 page 对应的地址中，代码类似如下:

```rust
let mut bounce_buffer = vec![0u8; pages_to_read.len() * PAGE_SIZE];
// big chunck IO request
file.read_at(bounce_buffer, start_pn as u64 * PAGE_SIZE as u64)?;


for &pn in &pages_to_read {
	// desigenate to each page
}
```

用中间缓冲区实现了 big chunk 读取之后 ([commit](https://github.com/uchouT/arceos/commit/1abc31bbfb3b20ae647a0c295fa33c6f793563f5)) 可以看到速度确实快了很多：

```
seq_read_test: (g=0): rw=read, bs=(R) 4096B-4096B, (W) 4096B-4096B, (T) 4096B-4096B, ioengine=psync, iodepth=1
fio-3.39
Starting 1 process
Jobs: 1 (f=1)
seq_read_test: (groupid=0, jobs=1): err= 0: pid=10583: Wed Dec 10 19:21:33 2025
  read: IOPS=27.0k, BW=105MiB/s (110MB/s)(128MiB/1215msec)
    clat (usec): min=6, max=3255, avg=33.10, stdev=79.29
     lat (usec): min=7, max=3255, avg=33.59, stdev=79.35
    clat percentiles (usec):
     |  1.00th=[    8],  5.00th=[    8], 10.00th=[    8], 20.00th=[    8],
     | 30.00th=[    8], 40.00th=[    8], 50.00th=[    8], 60.00th=[    8],
     | 70.00th=[    9], 80.00th=[   18], 90.00th=[   82], 95.00th=[  202],
     | 99.00th=[  396], 99.50th=[  424], 99.90th=[  537], 99.95th=[  619],
     | 99.99th=[ 1221]
   bw (  KiB/s): min=106519, max=109398, per=100.00%, avg=107958.50, stdev=2035.76, samples=2
   iops        : min=26629, max=27349, avg=26989.00, stdev=509.12, samples=2
  lat (usec)   : 10=76.94%, 20=7.84%, 50=2.09%, 100=3.81%, 250=5.90%
  lat (usec)   : 500=3.27%, 750=0.12%, 1000=0.01%
  lat (msec)   : 2=0.01%, 4=0.01%
  cpu          : usr=16.95%, sys=82.96%, ctx=0, majf=0, minf=0
  IO depths    : 1=100.0%, 2=0.0%, 4=0.0%, 8=0.0%, 16=0.0%, 32=0.0%, >=64=0.0%
     submit    : 0=0.0%, 4=100.0%, 8=0.0%, 16=0.0%, 32=0.0%, 64=0.0%, >=64=0.0%
     complete  : 0=0.0%, 4=100.0%, 8=0.0%, 16=0.0%, 32=0.0%, 64=0.0%, >=64=0.0%
     issued rwts: total=32768,0,0,0 short=0,0,0,0 dropped=0,0,0,0
     latency   : target=0, window=0, percentile=100.00%, depth=1

Run status group 0 (all jobs):
   READ: bw=105MiB/s (110MB/s), 105MiB/s-105MiB/s (110MB/s-110MB/s), io=128MiB (134MB), run=1215-1215msec
```

但是这样会引入不必要的拷贝，为什么不能在 IO 设备搬运数据时，直接通过 DMA 将数据搬运到每个 page 所在的位置呢？查阅相关资料，发现了 [scatter - gather](https://en.wikipedia.org/wiki/Gather/scatter_(vector_addressing))，这能够在一次调用内同时处理多个缓冲区，这正是消除中间缓冲区所必须的。

检查 StarryOS api 的时候发现了 `sys_readv` 这个系统调用，它能做到内核到用户空间的 *scatter - gather*：

```rust
// StarryOS/api/src/syscall/fs/io.rs

pub fn sys_readv(fd: i32, iov: *const IoVec, iovcnt: usize) -> AxResult<isize> {
    debug!("sys_readv <= fd: {fd}, iovcnt: {iovcnt}");
    let f = get_file_like(fd)?;
    f.read(&mut IoVectorBuf::new(iov, iovcnt)?.into_io().into())
        .map(|n| n as _)
}
```

不过这与我们的目的没有任何关系：我们的目的是实现从 IO 设备到内核态 page cache 的 *scatter - gather*。

很可惜，经过初步尝试，要在现有的接口基础上实现 scatter - gather，需要大量重构，还要修改驱动的代码。这一步暂时先搁置，在未来实现。

不过我们可以进一步优化实现：将 bounce_buffer 复用，从而减少 vec 的分配销毁开销：

```diff
 struct CachedFileShared {
     page_cache: Mutex<LruCache<u32, PageCache>>,
+    bounce_buffer: Mutex<Vec<u8>>,
     evict_listeners: Mutex<LinkedList<EvictListenerAdapter>>,
 }
```

可以看到速度确实又进一步提升了：

```
starry:~# time dd if=/largefile of=/dev/null bs=1M count=65536
128+0 records in
128+0 records out
134217728 bytes (128.0MB) copied, 0.873597 seconds, 146.5MB/s
real    0m 0.91s
user    0m 0.00s
sys     0m 0.1717986s
starry:~# time dd if=/largefile of=/dev/null bs=1M count=65536
128+0 records in
128+0 records out
134217728 bytes (128.0MB) copied, 0.888497 seconds, 144.1MB/s
real    0m 0.93s
user    0m 0.00s
sys     0m 0.1717986s
starry:~# time dd if=/largefile of=/dev/null bs=1M count=65536
128+0 records in
128+0 records out
134217728 bytes (128.0MB) copied, 0.894203 seconds, 143.1MB/s
real    0m 0.93s
user    0m 0.00s
sys     0m 0.1717986s
```

完整的实现在 [feat(fs): introduce bounce buffer cache](https://github.com/uchouT/arceos/commit/bc8ceb051908e10dce50af738417d8dc5abd8c4c)

## 引入 pending page

对于第二个方向, 思路是在 page cache 中引入 pending page flag，在 async readahead 中，先插入到 lrucache 中，这样消费窗口追上后，不会立刻判定为 cache miss 从而执行重复的预读，而是等待异步预读完成，将 pending page flag 消除，然后填入想要的数据。

这部分的初步实现由同伴 [aster](https://github.com/Aster-amellus) 完成：[perf(fs):optimize readahead strategy for mixed workload](https://github.com/Aster-amellus/arceos/commit/3e32c21b8d6a3a263df9c7a3b32020c977c3043a)

我在此基础上修复了遇到 pending page 后，如果这个 pending page 带 pg_readahead flag 不会触发 async readahead 的问题。以及优化了 in_memory 情况下 io_submit 逻辑。

不过现在发现了一个残酷的真相：async readahead 并不会提高读取的性能，现在的性能提升主要来自大块读取优化，以及启发式窗口更新。

同伴的实现中，修改了 async readahead 的触发条件, 用户请求窗口过大和过小都不会触发异步预读逻辑。而在测验中，不触发异步预读逻辑的情况下，速度提升非常巨大；而触发异步预读逻辑的情况下，速度提升相对较小 ( 这里比较的对象是同样开启异步预读，但是没有引入 pending page )。而如果直接关闭所有的异步预读，所有情况都能获得大幅性能提升。

可见，在异步预读实现纵向对比中，加入 pending page flag 确实可以提高性能，但是横向对比不开启异步预读的情况，异步预读的效果是削弱性能的。

主要原因是，当前 StarryOS 和 arceos 中，切换开销非常大，要大于 io 请求的开销，而这在现实情况中不是很常见，因此进一步探索 starryOS 的调度机制也是我们未来的工作之一。

