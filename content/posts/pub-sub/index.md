---
title: "sub-pub 发布订阅模式"
date: 2025-04-01T22:26:55+08:00
author: ["uchouT"]
showLastMod: true

categories:
- 学习笔记

tags:
- SoftWare

keywords:
- SoftWare
- publish-subscribe-pattern
- 发布订阅模式
- 软件设计模式

description: "" # 文章描述，与搜索优化相关
summary: "本文记录学习发布订阅模式时，自己的一些理解。" # 文章简单描述，会展示在主页
weight: # 输入1可以顶置文章，用来给文章展示排序，不填就默认按时间排序
slug: ""
draft: false # 是否为草稿
comments: true
showToc: true # 显示目录
TocOpen: true # 自动展开目录

disableShare: true # 底部不显示分享栏
searchHidden: false # 该页面可以被搜索到
showbreadcrumbs: false #顶部显示当前路径
mermaid: false
cover:
    image: ""
    caption: ""
    alt: ""
    relative: false
---

本文记录学习发布订阅模式时，自己的一些理解。[Wiki](https://en.wikipedia.org/wiki/Publish%E2%80%93subscribe_pattern)

>In software architecture, publish–subscribe or pub/sub is a messaging pattern where publishers categorize messages into classes that are received by subscribers. This is contrasted to the typical messaging pattern model where publishers send messages directly to subscribers.

## 组成

### 事件总线

接收**发布者**发布的事件，进行广播，通知**订阅者**执行回调。存储订阅信息。

### 发布者

生成发布事件，提供 **event**。

### 订阅者

接收**发布者**通过**事件总线**发布的 **event**，并回调对应 **event** 的方法。一个 **event** 对应唯一的一系列需要回调的方法，因此可以通过字典来存储。

## 示例 

C++ 代码：

```cpp
#include <iostream>
#include <functional>
#include <unordered_map>
#include <vector>

class EventBus {
public:
    using Callback = std::function<void(const std::string&)>;

    // 订阅事件
    void subscribe(const std::string& event, Callback callback) {
        subscribers[event].push_back(callback);
    }

    // 发布事件
    void publish(const std::string& event, const std::string& message) {
        if (subscribers.find(event) != subscribers.end()) { // 检索事件是否存在
            for (const auto& callback : subscribers[event]) {
                callback(message);  // 调用订阅者的回调
            }
        }
    }

private:
    std::unordered_map<std::string, std::vector<Callback>> subscribers;
};

// 示例用法
int main() {
    EventBus bus;

    // 订阅事件
    bus.subscribe("message", [](const std::string& msg) {
        std::cout << "订阅者1收到消息: " << msg << std::endl;
    });

    bus.subscribe("message", [](const std::string& msg) {
        std::cout << "订阅者2收到消息: " << msg << std::endl;
    });

    // 发布事件
    bus.publish("message", "Hello, C++!");

    return 0;
}

```

说明：

1. 事件总线类需要同时拥有 pub 和 sub，以便统一进行广播。
2. `std::function` 类型存储一个函数，可在需要时回调。

## 思考

发布-订阅模式，非常适用于某一事件发生，需要有一些回应的设计场景。
### 订阅事件

本质上是订阅者声明对某个事件 event 感兴趣，并将事件 event 发生时要进行的操作（回调函数）记录进事件总线。

```cpp
bus.subscribe("message", [](const std::string& msg) {
        std::cout << "订阅者1收到消息: " << msg << std::endl;
    });
```

这里的 "message" 即为对应的事件 event，后面的 lambda 函数即为名为 "message" 的 event 发生时，要进行的响应函数。lambda 函数的形参用来响应事件传达的消息。

### 发布事件

本质上是通过事件总线，调用所有对应的订阅者的回调函数。

```cpp
bus.publish("message", "Hello, C++!");
```

这里的 "message" 即为对应的事件。发布后，所有先前声明对 "message" 这一事件感兴趣的订阅者，都会进行响应。而后面的 "Hello, C++!" 是事件的消息，同一个事件种类，可以有多种消息，订阅者进行的操作（回调函数）根据消息可进行不同的响应。

### 松耦合特性

如上面提到的，订阅事件只是声明对某一个事件 event 感兴趣，发布者并不会参与其中。而发布事件，只是在事件总线中进行广播，通知对应事件 event 要发生，而不关心实际订阅者。
