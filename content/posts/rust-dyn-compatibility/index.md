+++
title = "Deep Dive into Rust Dyn Compatibility"
description = "深入解析 Rust Dyn Compatibility (Object Safety) 及其背后的 VTable 机制。从栈内存的 Sized 限制讲起，剖析 Trait Object、Fat Pointer 与 Type Erasure 的底层实现，揭示泛型、关联类型与 Self 引用破坏动态分发的物理原因，并对比 Enum Dispatch 的设计权衡。"
date = 2026-01-11
updated = 2026-05-09
draft = false

[taxonomies]
categories = ["Learning"]
tags = ["Rust", "programming"]
+++

## Intro

前些日子看了 [Let's Get Rusty](https://youtu.be/9RsgFFp67eo) 关于 `async trait` 的视频, 质量相当高, 覆盖了 Rust 的多个特性。其中就有提到 [dyn compatibility](https://doc.rust-lang.org/reference/items/traits.html#r-items.traits.dyn-compatible)。本文想进一步深入这个主题。从根本上理解 dyn compatibility 存在的原因。

---

## The Sized Constraint

为了搞清楚 dyn compatibility 那些看似繁杂的规则, 我们必须回到计算机的底层。在多态中, 我们主要关注的是如何调用对象的方法。为了理解这一点, 我们首先得深入 Rust 函数调用的一个**物理限制**。

函数运行时把变量保存在栈上。保存在栈上的数据高度紧凑, 可以提高空间利用率, 并且利于 CPU 缓存, 这也是现代程序可以高性能运行的基础之一。不过这高性能的代价是, 栈上的数据必须是**大小确定 (Sized)** 的。

{% note(title="Note") %}
尽管 C 语言支持 VLA 这种运行时变长数组, 但 Rust 出于安全和性能考虑, 默认要求栈变量必须是编译期确定大小
{% end %}

这很好想象, 因为在栈中数据之间是高度紧凑的, 如果其中一个数据是大小可变的, 那么当其变大时, 与其紧挨着的数据就会被覆盖; 当其变小时, 就会产生内存空洞, 造成空间上的浪费。

并且, 变量最终需要被加载到 CPU 的寄存器中才能被进一步执行。而寄存器大小是有限的, 而且很小的, 这也进一步说明栈上的变量必须是大小确定的。

可变长度的内容保存在堆上, 在栈上通过指针的方式访问。而指针就是一种大小确定的数据, 在 64 bit 计算机上通常为 8 bytes (瘦指针) 或者 16 bytes (胖指针)。我们常见的可变数组 `Vec` 在底层实现中就有维护一个指针, 实际的数据并没有存放在这个结构体中。因此 `Vec` 本身的大小是确定的, 可以存放在栈上。

[WHY IS STACK SO FAST?](https://youtu.be/N3o5yHYLviQ) 这个视频也很清晰地阐述了这一点。

因此我们可以知道, Rust 函数的**参数**以及**返回值**大小必须是可以确定的。注意, 这里不存在例外。一个不定长的数据, 一定是以某种指针的形式被访问的, 比如 `&` 引用, `Box`, etc.

```rust
// fn func(a: str) {}  Compile Error

fn func(a: &str) {}  // YES
```
---

## Dyn Compatibility Rules

现在我们就可以解释部分 The Ref 中关于 dyn compatibility 的表述了。

我们知道 `dyn SomeTrait` 本身是 `!Sized` 的。如果 Trait 要求 `Self: Sized`，那么 `dyn SomeTrait` 就因无法满足这个约束而无法存在:

```rust
trait SomeTrait: Sized {/* ... */} // Will lose dyn compatibility
```
对应的 The Ref 表述为:

> * `Sized` must not be a supertrait. In other words, it must not require `Self: Sized`.

---

不过我们可以把约束放在方法里, 这样相当于显式声明这个方法只能用在具体类型中。这实际上是告诉编译器: **这个方法不需要进入 vtable**。既然不进入 vtable, 也就无法通过 trait object 进行动态分发, 自然也就不必遵守 dyn compatibility 的规则了。

```rust
trait SomeTrait {
    fn method_a(&self) where Self: Sized;
    fn method_b(&self);
}
```
```rust
fn main() {
    let obj: &dyn SomeTrait = get_obj();
    // obj.method_a(); Compile Error, method not exist in trait object
    obj.method_b(); // Other methods work
}
```
而函数的参数以及返回值大小必须是确定的, 我们不能将 `Self` 作为 trait method 的参数或返回值, 必须用指针或者引用。

{%warning(title="Warning")%}
并不是任意的指针包装都被允许 (see [arbitrary_self_types](https://github.com/rust-lang/rust/issues/44874))
{%end%}

```rust
trait SomeTrait {
    // fn method(self); Will lose dyn compatibility
    fn method(&self);
}
```
这在 The Ref 中体现如下:

> * All associated functions must either be dispatchable from a trait object or be explicitly non-dispatchable:
>     * Dispatchable functions must:
>         * Be a method that does not use `Self` except in the type of the receiver.
>         * Have a receiver with one of the following types:
>             * `&Self` (i.e. `&self`)
>             * `&mut Self` (i.e `&mut self`)
>             * `Box<Self>`
>             * `Rc<Self>`
>             * `Arc<Self>`
>             * `Pin<P>` where `P` is one of the types above
>         * ...
>     * Explicitly non-dispatchable functions require:
>         * Have a `where Self: Sized` bound (receiver type of `Self` (i.e. `self`) implies this).

值得注意的是, The Ref 这里的描述隐含了两个并不那么显而易见的强限制:
1. **必须有 receiver**: Dispatchable function 必须带有 `self` 参数。这意味着静态方法(关联函数)无法通过 trait object 调用。
2. **`Self` 只能出现在 receiver 中**: `Self` 不能作为其他参数的类型, 也不能作为返回值类型。

为什么会有这些限制？如果我们从 Rust 如何实现动态分发 (Dynamic Dispatch) 的角度来看, 这一切就变得理所当然了。这就是我们接下来我们要探讨的核心机制 —— **VTable**。

---

## The Mechanism of Dynamic Dispatch

运行时多态的核心在于用一种策略应对**各种可能的具体类型**。因为 Trait Object 的具体类型在当前上下文中是不确定的（或只有在运行时才确定），而程序的执行步骤（即 CPU 指令）是在编译时就确定的。也就是说，我们需要一种方法，能够用**一套确定的 CPU 指令**来操作所有可能符合条件的 Trait Object。

现在我们来探讨本文的核心：**Dyn Compatibility**。这个 *dyn* 到底是什么？它是如何做到运行时多态的？

我们都知道，`&dyn SomeTrait` 实际上是一个胖指针（Fat Pointer）。那么为什么一个胖指针就可以让编译器生成统一的代码，来应对运行时各种不同的具体类型呢？

这里的核心机制是**类型擦除 (Type Erasure)**。这是通过 Unsize Coercion 实现的，它将具体类型的指针转换为了统一的胖指针结构。

一个胖指针的内部结构大致如下：

```rust
// 示意图，在内存布局上等同于两个指针
struct DynTraitObject {
    data: *mut (),   // 指向具体数据的指针 (类型信息被擦除，视作 void*)
    vtable: *const (), // 指向虚函数表及其它元信息 (size, align, drop...)
}
```

这里的 `data` 字段彻底隐藏了具体类型信息（Type Erasure），调用者只把它当作一个不透明的地址。而所有的类型信息都存储在 `vtable` 中。

[vtable](https://en.wikipedia.org/wiki/Virtual_method_table) 本质上是一个函数指针数组，加上一些元数据。对于一个 Trait，编译器会为每一个实现了该 Trait 的具体类型（Concrete Type）生成一个全局唯一的 vtable。对于实现同一个 Trait 的类型, 生成的 vtable 内存布局是完全一致的, 因此计算机才可以用统一的指令来应对运行时多态。

比如我们可以把所有实现 `SomeTrait` 的类型生成的 vtable 想象为:

```rust
struct SomeTraitVtable {
    // 1. metadata
    drop: fn(*mut ()), // 析构函数指针
    size: usize,       // 具体类型的大小
    align: usize,      // 具体类型的对齐方式

    // 2. trait method pointers
    method_a: fn(*mut (), ...), 
    method_b: fn(*mut (), ...),
    // ...
}
```

这里我们能看清一个关键点：**vtable 必须是一个编译期确定大小的结构体**。这就意味着，只有当 Trait 的定义能让编译器生成一个**确定布局 (Static Layout)** 的 vtable 时，这个 Trait 才是 Dyn Compatible 的。

如果 Trait 中包含任何无法生成这种统一 vtable 的特性（下面会细讲），它就不能用于构建 Trait Object。

从编译器视角, dyn compatibility 的本质就是: 编译器能否自动为 `dyn SomeTrait` 合成一个 `impl SomeTrait for dyn SomeTrait`, 把每次调用通过 vtable 正确分发。事实上, 对于 dyn-compatible 的 trait, 编译器确实会自动生成这样的 impl, 这也是为什么我们可以直接在 `&dyn SomeTrait` 上调用 trait 方法。

```rust
// 编译器会为 dyn-compatible 的 trait 自动合成类似下面的 impl
impl SomeTrait for dyn SomeTrait {
    /*...*/
}
```

所以, 判断一个 trait 是否 dyn-compatible 时, 可以自己想象手写这个 impl, 有没有足够的信息把每个方法都正确地分发出去。

这样就可以来解释一个具体类型在运行时多态的过程了:

```rust
struct SomeTraitImpl;

impl SomeTrait for SomeTraitImpl {
    /*...*/
}

fn dyn_dispatch(some_trait_obj: &dyn SomeTrait) {
    some_trait_obj.method_a();
    /*...*/
}
```
在使用 `dyn_dispatch` 方法时，我们将一个具体类型的引用（比如这里的 `&SomeTraitImpl`）传入函数。此时会发生 Unsize Coercion，将其转化为胖指针：
1. `data`：保存指向 `SomeTraitImpl` 实例的内存地址（原先的瘦指针）。
2. `vtable`：指向编译器为 `SomeTraitImpl` 静态生成的只读 `SomeTraitVtable`。

当通过胖指针调用方法 `some_trait_obj.method_a()` 时，运行时会先通过 `vtable` 找到对应的函数指针，然后将 `data` 作为第一个参数传入。这个具体的函数在内部清楚地知道该如何处理这个 `data` 指针（例如将其强转回 `&SomeTraitImpl`），从而正确地操作数据。

---
## The Barriers to Dyn Compatibility

知道 *dyn* (vtable) 是什么后, 我们就可以来讨论 **Dyn Compatibility** 了。能否拥有 **Dyn Compatibility**, 关键在于能否构建出与 trait 相对应的一个统一的 vtable 结构体。

我们重点来看 vtable 结构中 trait method pointers。为了能构建出统一的 vtable, **Trait Object** 的 trait method 必须数量结构都相同。那么所有会破坏 trait method 结构的东西, 都会破坏 vtable 的构建。

### 1. Associated Constants

因为 vtable 本质上只设计用于存储函数指针和统一的元数据。而 Associated Constants 是具体的数据值，其类型和大小在不同的实现中可能完全不同，编译器无法在 vtable 中为这种差异化的静态数据预留统一的存储空间。

### 2. Generics and Associated Types

这主要包括带有泛型参数的方法，以及泛型关联类型 (GATs)。

在 Rust 中，泛型是通过**单态化 (Monomorphization)** 实现的：编译器会为每一个用到的泛型参数生成一份专门的代码。这意味着一个泛型方法 `fn method<T>` 实际上代表了无限种可能的具体函数（如 `method_u8`, `method_string`, ...）。

在构建 vtable 时，编译器需要确定表的大小和每一项的确切位置。由于无法预知未来会以什么类型参数调用这个方法，也无法将无限种可能的函数指针都放入一个固定大小的结构中，因此**泛型方法无法进入 vtable**。

不过正如前面所说的, 我们可以通过 `where Self: Sized` 约束显式将某个方法从 vtable 中剔除，从而保留 Trait 的 **Dyn Compatibility**：

```rust
trait SomeTrait {
    // 加上 where Self: Sized 后，该方法不会出现在 vtable 中
    // 因此 trait 仍然保持 Dyn Compatibility（只是通过 Trait Object 无法调用此方法）
    fn method<T>(&self) -> T where Self: Sized; 
    
    // 如果不加约束，编译器试图将其放入 vtable 却做不到，
    // 导致整个 Trait 失去 Dyn Compatibility
    // fn method<T>(&self) -> T; 
}
```
值得注意的是, **trait 定义上的泛型参数**是允许的，因为 **Trait Object** 本身也是单态化的：

```rust
trait SomeTrait<T> {
    fn method(&self) -> T; // 这里的 T 是 trait 定义的一部分，已确定
}
```
但这意味着 `SomeTrait<i32>` 与 `SomeTrait<i64>` 是完全不同的两个 Trait。因此不存在通用的 `&dyn SomeTrait`, 只有具体的 `&dyn SomeTrait<i32>` 或 `&dyn SomeTrait<i64>`。

3. 普通 Associated Types

普通的关联类型（不带泛型）本身不破坏 **Dyn Compatibility**。但由于 vtable 中的函数签名必须是确定的，而关联类型会影响返回值或参数的类型，因此在使用 **Trait Object** 时必须显式指定关联类型的值：

```rust
trait SomeTrait {
    type SomeType;
    fn get(&self) -> Self::SomeType;
}
```
*   `&dyn SomeTrait` 是不合法的（编译器不知道 `get` 函数返回多大的数据）。
*   `&dyn SomeTrait<SomeType = i32>` 是合法的（编译器知道 `get` 返回 `i32`）。

这在 The Ref 中的表述如下:

> * It must not have any associated constants.
> * It must not have any associated types with generics.
> * All associated functions must either be dispatchable from a trait object or be explicitly non-dispatchable:
>     * Dispatchable functions must:
>         * Not have any type parameters (although lifetime parameters are allowed).
>         * ...

### 3. The Self Type

我们再来回看 [The Sized Constraint](#the-sized-constraint) 中留下的问题:

> * Be a method that does not use `Self` except in the type of the receiver.

在 Rust 中, trait 里的 `Self` 指向 trait 实现者类型, 每个实现者的 `Self` 类型都不相同, 也就无法统一 trait method 的结构了, 所以下面的这些方法都没有 dyn compatibility:

```rust
trait SomeTrait {
    // Self 不作为引用传入, 上面 Size 中也论证过这是不可行的
    fn method_a(self);

    fn method_b(&self, other: Self);

    fn method_c(&self) -> Self;
}
```
---
那么如果用 `&Self` 指针的形式呢？从底层 ABI 的角度来看，所有具体类型的引用（如 `&String`, `&u8`）本质上都是一个 64 位的指针。这意味着，仅仅从**生成统一的 vtable 结构**这一物理角度来看，似乎是可以做到的。

```rust
trait SomeTrait {
    // 物理上可以生成统一的函数指针签名 fn(*mut (), *mut ())
    fn method_a(&self, other: &Self);
    
    // 物理上可以生成统一的函数指针签名 fn(*mut ()) -> *mut ()
    fn method_b(&self) -> &Self;
}
```
**但为什么依然不行？问题不在于 vtable 的物理布局，而在于类型系统的安全性与逻辑闭环。**

* **对于参数 (`other: &Self`)**：虽然是指针传递，但编译器必须保证安全性。如果在 `dyn Trait`（比如指向 `Cat`）上调用方法，传入了另一个 `dyn Trait`（比如指向 `Dog`），虽然都是指针，但函数内部会把 `Dog` 的指针强转成 `Cat` 来访问，直接导致内存访问错误。而由于类型擦除，编译器无法在编译期通过静态检查阻止这种行为，因此只能从规则上禁止。
* **对于返回值 (`-> &Self`)**：虽然返回的也是指针，但这会导致类型系统的死锁。调用者的上下文中只有 `dyn Trait`，原本的具体类型 `Self` 已经被擦除。即使函数底层成功返回了一个指针，调用者也无法用具体的类型去定义变量来接收它（上下文中不存在 `Self` 这一类型）。

但是为什么 receiver 可以有 `Self` ？ 回顾 dyn 的过程:

> 当通过胖指针调用方法 `some_trait_obj.method_a()` 时，运行时会先通过 `vtable` 找到对应的函数指针，然后将 `data` 作为第一个参数传入。这个具体的函数在内部清楚地知道该如何处理这个 `data` 指针（例如将其强转回 `&SomeTraitImpl`），从而正确地操作数据。

```rust
struct DynTraitObject {
    data: *mut (),   // 这里存的就是那个 Self 实例的地址
    vtable: *const (),
}
```

当我们发起动态分发调用时，编译器生成的指令实际上做了这样一件事：

1. 从 `vtable` 中取出对应的函数指针。
2. 将 **`data` 指针**（也就是被擦除类型的 `self`）作为第一个参数传进去。

也就是说，接收者位置（Receiver）的 `Self` 在动态分发过程中，**正好对应着胖指针中的 `data` 字段**。

因此，只有 Receiver 位置的 `Self` 是特例，因为它的类型擦除和指针传递正是 Dynamic Dispatch 机制的核心工作。

---
### 4. Opaque Return Types

现在来看整个 The Ref 对 dyn compatibility 的描述:

> A dyn-compatible trait can be the base trait of a trait object. A trait is
> *dyn compatible* if it has the following qualities:
> 
> * All supertraits must also be dyn compatible.
> 
> * `Sized` must not be a supertrait. In other words, it must not require `Self: Sized`.
> 
> * It must not have any associated constants.
> 
> * It must not have any associated types with generics.
> 
> * All associated functions must either be dispatchable from a trait object or be explicitly non-dispatchable:
>     * Dispatchable functions must:
>         * Not have any type parameters (although lifetime parameters are allowed).
>         * Be a method that does not use `Self` except in the type of the receiver.
>         * Have a receiver with one of the following types:
>             * `&Self` (i.e. `&self`)
>             * `&mut Self` (i.e `&mut self`)
>             * `Box<Self>`
>             * `Rc<Self>`
>             * `Arc<Self>`
>             * `Pin<P>` where `P` is one of the types above
>         * Not have an opaque return type; that is,
>             * Not be an `async fn` (which has a hidden `Future` type).
>             * Not have a return position `impl Trait` type (`fn example(&self) -> impl Trait`).
>         * Not have a `where Self: Sized` bound (receiver type of `Self` (i.e. `self`) implies this).
>     * Explicitly non-dispatchable functions require:
>         * Have a `where Self: Sized` bound (receiver type of `Self` (i.e. `self`) implies this).
> 
> * The `AsyncFn`, `AsyncFnMut`, and `AsyncFnOnce` traits are not dyn-compatible.

除了 Opaque return type 相关的, 其他都已经被解释了 (Future 相关的内容可以看 [Let's Get Rusty](https://youtu.be/9RsgFFp67eo) 的这个视频, 在本文中, 你只需知道 `async fn` 最终也会生成一个 `impl Trait` 的返回类型)。

Opaque return type 只针对于 `impl Trait` 作为返回值, 如果作为参数, 这两者是等价的:

```rust
fn method_a<T: SomeTrait>(a:T);
fn method_b(a: impl SomeTrait);
```
而在前面我们已经说明了, trait method 中不允许有泛型。

`impl Trait` 作为返回值时, 实际上是一个不透明类型 (Opaque type)。这里的“不透明”是相对于调用者而言的：调用者不知道具体类型是什么，但编译器在编译时是完全清楚其背后的具体类型的。

简而言之，`-> impl Trait` 只是**对具体返回类型的隐藏**，而不是动态分发。它在编译期就已确定为某一种单一类型, 因此**不具备分发性**。

因此，`impl Trait` 返回值（以及 `async fn`）会导致 Trait 无法构建统一的 vtable，从而丧失 Dyn Compatibility。

---
## Static Dispatch

至此，我们已经深入探讨了 Dynamic Dispatch 的原理及其限制。现在，让我们换个角度：如果我们能在编译期就确定**所有可能的类型集合**呢？

在 [The Mechanism of Dynamic Dispatch] 我们提到:

> 运行时多态的核心在于用一种策略应对**各种可能的具体类型**。

这里的「各种可能的具体类型」如果是确定的, 有限的, 那么我们也就可以用确定性的 CPU 指令来描述分发了。比如我们可以利用 `enum` 将这些类型包裹起来，通过高效的 `match` 语句来进行分发。

这就是 **Enum Dispatch**。事实上, Rust 就有一个 crate 叫做 [enum_dispatch](https://docs.rs/enum_dispatch/latest/enum_dispatch) 具体的原理在其文档中已经讲的很清晰了, 这里也就不再赘述。

`enum` static dispatch 和 `dyn` dynamic dispatch 都属于**运行时分发**。

但前者具有极高的性能（利于 CPU 分支预测和内联）。不过代价是失去了灵活性：也就是**失去了[开闭原则 (Open-Closed Principle)](https://en.wikipedia.org/wiki/Open%E2%80%93closed_principle)**。它的运行时分发只限于编译时确定的具体类型。

而 dynamic dispatch 通过类型擦除, 从而拥有**无限**的灵活性: 任何实现了该 trait 的类型, 都可以参与动态分发。当然了前提是 trait 具有 dyn compatibility。这样就可以在底层库中定义 trait 以及 trait 相关的使用方法。而又高层调用者提供 trait 的具体实现类型。这就是我们常说的依赖倒置原则。

## Summary

回顾全文, Rust 的 Dyn Compatibility 并不是一堆随意的语法规定，而是为了适配底层硬件限制而做出的自然选择。

1.  **物理限制**: 栈内存的高效利用要求变量必须是 **Sized**。
2.  **类型擦除**: 为了让不同大小的具体类型能以统一的方式被调用，我们必须使用指针（`&`, `Box` 等），并擦除具体的类型信息。
3.  **VTable**: 被擦除的类型信息转移到了 **VTable** 中。Fat Pointer (`data` + `vtable`) 是实现动态分发的关键机制。
4.  **规则本源**: 无论是泛型方法的限制，还是 `Self` 类型的约束，本质上都是因为它们会导致编译器**无法生成统一的 VTable 布局**。

最后，关于运行时分发（Runtime Dispatch），我们有两种选择：

*   **Dyn Dispatch**: 牺牲少量性能（胖指针、间接调用）换取架构上的解耦（遵循开闭原则）。适用于库设计和需要依赖倒置的场景。
*   **Static Dispatch**: 牺牲灵活性换取极致性能（利于分支预测、内联）。适用于类型集合封闭、确定的场景。
