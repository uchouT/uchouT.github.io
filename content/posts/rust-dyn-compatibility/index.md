+++
title = "Deep Dive into Rust Dyn Compatibility"
description = "A deep dive into Rust's dyn compatibility (object safety). Starting from fat pointers and vtables, we reduce every rule in the Rust Reference to a single question: can the compiler write impl Trait for dyn Trait for you? Then we compare the trade-offs with enum dispatch."
date = 2026-01-11
draft = false

[taxonomies]
categories = ["Learning"]
tags = ["rust", "programming"]

[extra]
lang = "en"

discuss.x = "https://x.com/0x_uchouT/status/2100328404805628314"

+++

## Intro

A while ago I watched [Let's Get Rusty](https://youtu.be/9RsgFFp67eo)'s video on `async trait`. It's a really high-quality video that touches on quite a few Rust features, one of which is [dyn compatibility](https://doc.rust-lang.org/reference/items/traits.html#r-items.traits.dyn-compatible). In this post I want to dig deeper into that topic and understand, from first principles, why dyn compatibility exists at all.

The Rust Reference (The Ref) describes it as a checklist:

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

At first glance it looks like a pile of unrelated restrictions. But every item on this list comes down to one question:

**Can the compiler write `impl Trait for dyn Trait` for you?**

To see why, we first need to understand what a trait object is and how a call through it gets dispatched. Then we'll go through the checklist one rule at a time, trying to write that impl ourselves.

---

## The Sized Constraint

`dyn Trait` stands for "some type that implements `Trait`", and different implementors have different sizes. So `dyn Trait` has no size known at compile time: it's `!Sized`.

That matters because Rust needs to know, at compile time, how much space every local variable, parameter and return value takes. Each slot in a stack frame gets a fixed offset, and moving a value means copying a fixed number of bytes. A value whose size is only known at runtime doesn't fit into that model.

> [!NOTE]
> This is a language design choice rather than a physical limit: C supports VLAs (arrays whose length is decided at runtime) on the stack. Rust, for safety and performance reasons, requires stack values to have a size known at compile time.

Variable-length content lives on the heap and is accessed through a pointer. A pointer is data of a known size: on a 64-bit machine it's typically 8 bytes (thin pointer) or 16 bytes (fat pointer). `Vec`, the growable array we use all the time, maintains a pointer internally; the actual data isn't stored inside the struct itself. That's why `Vec` itself has a known size and can live on the stack.

The video [WHY IS STACK SO FAST?](https://youtu.be/N3o5yHYLviQ) explains how the stack works very clearly.

So in Rust, the sizes of a function's **parameters** and **return value** must be known (on stable Rust there are no exceptions; nightly has the `unsized_fn_params` feature). Data of unknown size is always passed through some kind of pointer, e.g. a `&` reference, `Box`, etc.

```rust
fn func(a: str) {}   // error

fn func(a: &str) {}  // ok
```

That's why trait objects always show up behind a pointer: `&dyn Trait`, `Box<dyn Trait>`, `Arc<dyn Trait>`.

---

## The Mechanism of Dynamic Dispatch

The essence of runtime polymorphism is having a single strategy that handles **every possible concrete type**. The concrete type behind a trait object is unknown in the current context (or only known at runtime), while the program's execution steps (i.e. the CPU instructions) are fixed at compile time. In other words, we need a way to operate on every eligible trait object with **one fixed set of CPU instructions**.

We all know that `&dyn SomeTrait` is actually a fat pointer. But why is a fat pointer enough for the compiler to generate uniform code that copes with all the different concrete types at runtime?

The core mechanism here is **type erasure**. It's achieved through unsize coercion, which converts a pointer to a concrete type into a uniform fat pointer structure.

Internally, a fat pointer looks roughly like this:

```rust
// Illustration only; in memory layout this is equivalent to two pointers
struct DynTraitObject {
    data: *mut (),   // Pointer to the concrete data (type info erased, treated as void*)
    vtable: *const (), // Pointer to the virtual function table and other metadata (size, align, drop...)
}
```

The `data` field completely hides the concrete type information (type erasure); the caller treats it as nothing more than an opaque address. All of the type information lives in the `vtable`.

A [vtable](https://en.wikipedia.org/wiki/Virtual_method_table) is essentially an array of function pointers plus some metadata. For a given trait, the compiler generates a vtable for each concrete type that actually gets converted into a trait object. The vtables generated for types implementing the same trait have exactly the same memory layout, which is what allows the machine to handle runtime polymorphism with a uniform set of instructions.

For example, we can picture the vtable generated for every type implementing `SomeTrait` as:

```rust
struct SomeTraitVtable {
    // 1. metadata
    drop: fn(*mut ()), // Destructor pointer
    size: usize,       // Size of the concrete type
    align: usize,      // Alignment of the concrete type

    // 2. trait method pointers
    method_a: fn(*mut (), ...), 
    method_b: fn(*mut (), ...),
    // ...
}
```

With that in place, we can walk through how a concrete type goes through runtime polymorphism:

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
When we call `dyn_dispatch`, we pass in a reference to a concrete type (here, `&SomeTraitImpl`). At this point an unsize coercion takes place, turning it into a fat pointer:
1. `data`: holds the memory address of the `SomeTraitImpl` instance (the original thin pointer).
2. `vtable`: points to the read-only `SomeTraitVtable` that the compiler statically generated for `SomeTraitImpl`.

When a method is called through the fat pointer, as in `some_trait_obj.method_a()`, the generated code first loads the corresponding function pointer from the `vtable`, then calls it with `data` as the first argument. That concrete function knows exactly how to handle the `data` pointer internally (e.g. by casting it back to `&SomeTraitImpl`), and so it operates on the data correctly.

---

## The Key Question: `impl Trait for dyn Trait`

From the type system's point of view, `dyn SomeTrait` is just a type. For `some_trait_obj.method_a()` to type-check, that type must implement `SomeTrait`. For a dyn-compatible trait, the compiler synthesizes this impl automatically, and every method in it simply forwards the call through the vtable:

```rust
trait SomeTrait {
    fn method_a(&self) -> String;
}

// Pseudo-code: what the compiler conceptually generates
impl SomeTrait for dyn SomeTrait {
    fn method_a(&self) -> String {
        // `self` is a fat pointer: (data, vtable)
        let (data, vtable) = split_fat_pointer(self);
        (vtable.method_a)(data)
    }
}
```

So **dyn compatibility boils down to whether this impl can be written**. And the compiler only has two things to write it with:

1. **At runtime**: the fat pointer, i.e. `data` plus `vtable`. It only has one if the call comes with a receiver.
2. **At compile time**: the type `dyn SomeTrait`, plus whatever is spelled out in it (like `dyn SomeTrait<SomeType = i32>`). The concrete type is gone.

Every rule in The Ref is a case where these two aren't enough. So whenever you wonder whether a trait is dyn compatible, try writing this impl by hand.

There's also an escape hatch: a method with a `where Self: Sized` bound can be skipped in this impl, because `dyn SomeTrait` is never `Sized`. Such a method is left out of the vtable and simply can't be called on a trait object, while the rest of the trait stays usable. We'll lean on this several times below.

---

## Walking Through the Rules

### 1. `Sized` Must Not Be a Supertrait

> * `Sized` must not be a supertrait. In other words, it must not require `Self: Sized`.

```rust
trait SomeTrait: Sized {/* ... */} // Will lose dyn compatibility
```

`impl SomeTrait for dyn SomeTrait` would require `dyn SomeTrait: Sized`, which can never hold. The impl is impossible before we even look at the methods.

Putting the bound on an individual method instead is exactly the escape hatch:

```rust
trait SomeTrait {
    fn method_a(&self) where Self: Sized;
    fn method_b(&self);
}
```
```rust
fn main() {
    let obj: &dyn SomeTrait = get_obj();
    obj.method_a(); // error: the `method_a` method cannot be invoked on a trait object
    obj.method_b(); // ok
}
```

### 2. No Receiver, No Vtable

> * Dispatchable functions must:
>     * Have a receiver with one of the following types: ...

In the impl, the vtable is reached through the receiver. An associated function without `self` gives the impl nothing to work with:

```rust
trait SomeTrait {
    fn create() -> u32; // no receiver
}

// Pseudo-code
impl SomeTrait for dyn SomeTrait {
    fn create() -> u32 {
        // No `self`, so no fat pointer and no vtable.
        // Which implementation should this call?
    }
}
```

Unless it has a `where Self: Sized` bound, such a function makes the whole trait lose dyn compatibility.

> * It must not have any associated constants.

Associated constants fail for the same reason. It's accessed as `<T as SomeTrait>::VALUE`, with no `self` involved. Given just the type `dyn SomeTrait`, there's no instance and therefore no vtable, so there's no way to tell at runtime which implementation's constant is meant. On top of that, a constant is expected to be known at compile time (e.g. usable as an array length), which a runtime vtable lookup could never provide anyway.

### 3. What Counts as a Receiver

Not every type can serve as a receiver. The impl has to pull the vtable out of the receiver and hand a thin pointer to the concrete function, so the compiler must know how to take that pointer type apart. That's why the list is limited to `&Self`, `&mut Self`, `Box<Self>`, `Rc<Self>`, `Arc<Self>` and `Pin<P>` of those. (Internally, the compiler tracks which pointer types support this through the unstable `DispatchFromDyn` trait.)

> [!WARNING]
> Not every pointer wrapper is allowed (see [arbitrary_self_types](https://github.com/rust-lang/rust/issues/44874))

What about a plain `self` taken by value? A receiver type of `Self` implies `where Self: Sized`, which means the trait doesn't lose its dyn compatibility, but calling this method on a trait object will fail:

```rust
trait Trait {
    fn method_a(&self);

    fn method_b(self);
}

struct TraitImpl;

impl Trait for TraitImpl {
    fn method_a(&self) {
        println!("method a");
    }

    fn method_b(self) {
        println!("method b")
    }
}

fn main() {
    let obj: &dyn Trait = &TraitImpl;
    obj.method_a();
    // ok, dyn compatibility isn't lost.
    
    obj.method_b();
    // error: the size of `dyn Trait` cannot be statically determined
}
```

### 4. `Self` Outside the Receiver

> * Be a method that does not use `Self` except in the type of the receiver.

First, why is `Self` in the receiver fine? Recall the fat pointer:

```rust
struct DynTraitObject {
    data: *mut (),   // This holds the address of that Self instance
    vtable: *const (),
}
```

When we make a dynamically dispatched call, the generated code does the following:

1. Fetch the corresponding function pointer from the `vtable`.
2. Pass the **`data` pointer** (i.e. the type-erased `self`) in as the first argument.

In other words, during dynamic dispatch the `Self` in receiver position **maps exactly onto the `data` field of the fat pointer**. Erasing its type and passing it by pointer is precisely the core job of the dynamic dispatch mechanism.

Everywhere else, remember that inside the impl, `Self` **is** `dyn SomeTrait`. Substitute it and see what happens:

```rust
trait SomeTrait {
    fn method_a(&self, other: Self);
    fn method_b(&self) -> Self;
}

// Pseudo-code
impl SomeTrait for dyn SomeTrait {
    fn method_a(&self, other: dyn SomeTrait) { /* ... */ } // unsized parameter
    fn method_b(&self) -> dyn SomeTrait { /* ... */ }      // unsized return value
}
```

As we saw in [The Sized Constraint](#the-sized-constraint), these signatures can't exist.

So what if we use a pointer, `&Self`? Now the signatures are perfectly writable:

```rust
// Pseudo-code
impl SomeTrait for dyn SomeTrait {
    fn method_a(&self, other: &dyn SomeTrait) { /* ... */ }
    fn method_b(&self) -> &dyn SomeTrait { /* ... */ }
}
```

The problem moves into the body.

* **For parameters (`other: &Self`)**: the function in the vtable was generated for one concrete type, say `Cat`, and expects `other` to be a `&Cat` as well. But `other` is an arbitrary `&dyn SomeTrait` and could just as well point to a `Dog`. Passing it along, the function would treat the `Dog` as a `Cat`, leading straight to invalid memory access. Because of type erasure, the compiler can't rule this out at compile time, so the only option is to forbid it.
* **For return values (`-> &Self`)**: this particular case could actually work. The concrete function returns a thin pointer to the same concrete type, so the impl could pair it with the receiver's vtable. But `Self` can appear anywhere in a signature: `-> Option<&Self>`, `-> Vec<Box<Self>>`, `other: HashMap<String, &Self>`. For those, the impl would have to turn thin pointers into fat pointers deep inside arbitrary data structures. A `Vec<Box<Cat>>` is not a `Vec<Box<dyn SomeTrait>>`; even their elements have different sizes. There's no general way to do that conversion, so the rule simply bans `Self` outside the receiver. This blanket ban goes back to [RFC 255](https://rust-lang.github.io/rfcs/0255-object-safety.html), which introduced object safety and left finer-grained rules as a possible future extension.

### 5. Generic Methods

> * Not have any type parameters (although lifetime parameters are allowed).

In Rust, generics are implemented through **monomorphization**: the compiler generates a dedicated copy of the code for every generic argument that's actually used. This means a generic method `fn method<T>` really stands for infinitely many possible concrete functions (`method_u8`, `method_string`, ...).

To forward such a method, the impl would need a vtable entry for every possible `T`. But the vtable is a fixed-size struct, and the compiler can't predict which type arguments the method will be called with in the future. So **generic methods can't go into the vtable**. Lifetime parameters are fine: they're erased before code generation, so they don't multiply the functions.

As before, `where Self: Sized` takes the method out of the picture:

```rust
trait SomeTrait {
    // With `where Self: Sized`, this method won't appear in the vtable,
    // so the trait stays dyn compatible (the method just can't be called through a trait object)
    fn method<T>(&self) -> T where Self: Sized; 
    
    // Without the bound, the impl for `dyn SomeTrait` can't be written,
    // and the whole trait loses dyn compatibility
    fn method<T>(&self) -> T; 
}
```

Note that `impl Trait` in argument position is just a generic in disguise. These two are equivalent:

```rust
fn method_a<T: SomeTrait>(a:T);
fn method_b(a: impl SomeTrait);
```

On the other hand, **generic parameters on the trait definition itself** are allowed:

```rust
trait SomeTrait<T> {
    fn method(&self) -> T; // T here is part of the trait definition and is already fixed
}
```

`SomeTrait<i32>` and `SomeTrait<i64>` are two different traits, so `dyn SomeTrait<i32>` and `dyn SomeTrait<i64>` are two different types, each with its own vtable layout. Inside `impl SomeTrait<i32> for dyn SomeTrait<i32>`, `T` is fixed, and the impl is easy to write. The price is that there's no general-purpose `&dyn SomeTrait`, only specific ones like `&dyn SomeTrait<i32>`.

### 6. Associated Types

> * It must not have any associated types with generics.

Plain associated types are allowed, with a catch:

```rust
trait SomeTrait {
    type SomeType;
    fn get(&self) -> Self::SomeType;
}

// Pseudo-code
impl SomeTrait for dyn SomeTrait {
    type SomeType = ???; // the concrete type has been erased
    fn get(&self) -> Self::SomeType { /* ... */ }
}
```

The impl has to define `SomeType`, and the only compile-time information it has is the `dyn` type itself. So we have to spell it out there:

*   `&dyn SomeTrait` is not valid: the impl has no way to fill in `SomeType`. The compiler rejects it even if no method uses `SomeType` at all, so this isn't about the size of `get`'s return value.
*   `&dyn SomeTrait<SomeType = i32>` is valid: now the impl can say `type SomeType = i32`.

Generic associated types (GATs) take this one step further. With `type Item<T>`, the `dyn` type would have to specify `Item<T>` for every possible `T`, and `dyn` types have no way to express that. So a GAT makes the trait lose dyn compatibility.

### 7. Opaque Return Types

> * Not have an opaque return type; that is,
>     * Not be an `async fn` (which has a hidden `Future` type).
>     * Not have a return position `impl Trait` type (`fn example(&self) -> impl Trait`).

When `impl Trait` is used as a return type, it's an opaque type. "Opaque" here is from the caller's point of view: the caller doesn't know what the concrete type is, but the compiler knows full well which concrete type is behind it. And that hidden type is different for every implementor.

In other words, an opaque return type is essentially an **anonymous associated type**:

```rust
trait SomeTrait {
    fn example(&self) -> impl Display;
}

// Pseudo-code
impl SomeTrait for dyn SomeTrait {
    fn example(&self) -> ??? { /* ... */ }
}
```

For a named associated type, we could at least write `dyn SomeTrait<SomeType = i32>`. An opaque type has no name, so there's no way to pin it down, and the impl can't be written.

`async fn` is the same story. For this post, all you need to know is that an `async fn` ends up returning an `impl Future` (for the details, check out [Let's Get Rusty](https://youtu.be/9RsgFFp67eo)'s video). The common workaround is to return `Pin<Box<dyn Future<Output = T>>>` instead, which is essentially what the [async-trait](https://docs.rs/async-trait) crate generates for you.

### 8. Supertraits and the `AsyncFn*` Traits

> * All supertraits must also be dyn compatible.

For `trait Sub: Super`, a `dyn Sub` must also be usable as a `Super`, so the compiler has to write `impl Super for dyn Sub` as well (the supertrait's methods are stored in `dyn Sub`'s vtable too). If `Super` isn't dyn compatible, that impl is impossible, and so is `dyn Sub`.

> * The `AsyncFn`, `AsyncFnMut`, and `AsyncFnOnce` traits are not dyn-compatible.

The `AsyncFn*` traits return their futures through associated types: `AsyncFnOnce::CallOnceFuture`, and the GAT `AsyncFnMut::CallRefFuture<'a>`. Every async closure has its own unnameable future type, which is the same problem as in the previous two sections.

---

## Enum Dispatch

So far we've dug into how dynamic dispatch works and what its limits are. Now let's look at things from a different angle: what if we could determine **the full set of possible types** at compile time?

In [The Mechanism of Dynamic Dispatch](#the-mechanism-of-dynamic-dispatch) we said:

> The essence of runtime polymorphism is having a single strategy that handles **every possible concrete type**.

If "every possible concrete type" here is a known, finite set, then we can describe dispatch with a fixed set of CPU instructions as well. For example, we can wrap those types in an `enum` and dispatch with an efficient `match`.

This is **Enum Dispatch**. In fact, Rust has a crate called [enum_dispatch](https://docs.rs/enum_dispatch/latest/enum_dispatch) whose documentation already explains how it works very clearly, so I won't repeat it here.

Note that enum dispatch is not static dispatch. Just like `dyn` dynamic dispatch, it is a form of **runtime dispatch**: which branch gets taken can only be decided at runtime by reading the enum's tag. What static dispatch really means in Rust is generics: through monomorphization, the compiler decides at compile time exactly which function to call, so no dispatch happens at runtime at all.

Compared with dyn dispatch, enum dispatch is usually much faster (it's friendly to CPU branch prediction and inlining). The cost is lost flexibility — namely, it **gives up the [Open-Closed Principle](https://en.wikipedia.org/wiki/Open%E2%80%93closed_principle)**. Its runtime dispatch is limited to the concrete types known at compile time.

Dynamic dispatch, thanks to type erasure, works with an **open** set of types: any type that implements the trait can take part — provided, of course, that the trait is dyn compatible. Generics can also let a low-level library define a trait while higher-level callers supply the implementations (the Dependency Inversion Principle), but only when the concrete type is known at compile time. `dyn` is what lets us pick an implementation at runtime, or keep values of different types in one collection.

## Summary

Looking back over the whole post, Rust's dyn compatibility rules aren't a pile of arbitrary restrictions. They all follow from one question: **can the compiler write `impl Trait for dyn Trait`?**

1.  **Trait objects are unsized**: `dyn Trait` has no size known at compile time, so it lives behind a fat pointer (`data` + `vtable`).
2.  **Calls are forwarded through the vtable**: the compiler synthesizes `impl Trait for dyn Trait`, where every method loads its function pointer from the vtable and passes `data` along.
3.  **That impl only has two things to work with**: the fat pointer at runtime, and the `dyn Trait` type at compile time. The rules are exactly the cases where that isn't enough:
    *   No receiver (associated functions without `self`, associated constants): no fat pointer, so no vtable.
    *   `Self` outside the receiver, unspecified associated types, opaque return types: the signature depends on the erased concrete type.
    *   Generic methods and GATs: they would need infinitely many vtable entries or type specifications.
    *   A `Sized` supertrait or a supertrait that isn't dyn compatible: the impl is impossible from the start.
4.  **The escape hatch**: `where Self: Sized` takes a method out of the impl and the vtable, so the rest of the trait stays dyn compatible.

Finally, when it comes to runtime dispatch, we have two options:

*   **Dyn Dispatch**: trades a little performance (fat pointers, indirect calls) for an open set of types (following the Open-Closed Principle). Suited to library design and scenarios where implementations are chosen at runtime.
*   **Enum Dispatch**: trades flexibility for performance (friendly to branch prediction and inlining). Suited to scenarios where the set of types is closed and known.
