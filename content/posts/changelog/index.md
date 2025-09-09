---
title: "Github Action 自动生成 Changelog"
date: 2025-09-09T17:29:40+08:00
author: ["uchouT"]
showLastMod: true

categories:
- 折腾日记

tags:
- Software
keywords:
- Software
- Github
- Changelog
- auto
- Github-Action

description: "Github push 推送版本 tag 时自动生成 CHANGELOG.md 以及 Release 信息。" # 文章描述，与搜索优化相关
summary: "" # 文章简单描述，会展示在主页
weight: # 输入1可以顶置文章，用来给文章展示排序，不填就默认按时间排序
slug: ""
draft: false # 是否为草稿
comments: true
showToc: true # 显示目录
TocOpen: true # 自动展开目录

disableShare: true # 底部不显示分享栏
searchHidden: false # 该页面可以被搜索到
showbreadcrumbs: false #顶部显示当前路径
---

前阵子在开发一个小工具，希望自己可以学习了解软件从开发到发布的过程。

其中就有关 Github Release 信息和 Changelog 的编写。这一过程有一些繁琐，并且保持格式统一也有些令人疲惫，于是我开始寻找自动化生成 Changelog 的方案。
<!-- more -->
编写 Changelog 的原因可以查看: [keepachangelog](https://keepachangelog.com).

## Changelogen 存在的问题

很快，我在网上发现了 [changelogen](https://github.com/unjs/changelogen), 这个工具可以快速生成精美的 Changelog 并输出到 `CHANGELOG.md`，以及创建或者更新 Release 信息，几乎完美符合我的需求。

但是有几个问题的存在，使其与 Github Action 结合的体验非常糟糕:

### 1. 适用项目受限

从他可以自动更新 `package.json` 中的版本号这一功能可以看出，changelogen 主要适用于 Node.js 项目。关键是这个功能在用 `--release` 发布到 GitHub 无法取消，如果不是 Node.js 项目或者 `package.json` 版本号更新失败，就会直接退出，根本不发布。

### 2. 获取当前 Tag 的机制

changelogen 的使用场景发生在打 tag 之前，如果不用 `--from` 和 `--to` 来指定 commit，他默认生成离 HEAD 最近的 tag 到 HEAD 之间的 Changelog。

但是我们在使用 Github Action 时，一般用 tag 来作为触发条件，也就是说我们在期望触发 Action 时，已经打好了 tag, 如果这时候使用 changelogen，默认行为就是生成 from `current_tag` to `current_tag` 的 changelog，这当然是毫无意义的。

---

当然我们也可以选择在 GitHub Action 运行时手动提取出前后 Tag，然后用 `--from` 和 `--to` 指定 tag。但是这显然不够优雅，幸好该项目不算复杂并且结构清晰，修改源码非常容易:

## 修改

```ts
// ./src/git.ts
export async function getLastGitTag(cwd?: string) {
  try {
    return execCommand("git describe --tags --abbrev=0", cwd)
      ?.split("\n")
      .at(-1);
  } catch {
    // Ignore
  }
}

// ...

export function getCurrentGitTag(cwd?: string) {
  return execCommand("git tag --points-at HEAD", cwd);
}

```

从源码可以看出，changelogen 通过执行外部 git 命令来获取上一条 tag 与当前 tag。只需在获取上一条 tag 时，忽略当前 HEAD, 即可正确处理 HEAD 打完 tag 的情况:

```ts
export async function getLastGitTag(cwd?: string) {
  try {
    return execCommand("git describe --tags --abbrev=0 HEAD^", cwd)
      ?.split("\n")
      .at(-1);
  } catch {
    // Ignore
  }
}
```

然后我移除了关于自动打 tag，以及 `package.json` 版本号更新的功能，以获取对更多项目的兼容性。

详细修改可以查看这个 [Commit](https://github.com/unjs/changelogen/commit/19a4a55e9b3d64eca57791822c74acd8c059d6a1).

## 使用

现在打完 tag 后，使用 `--release` 参数就可以正确生成 Changelog 并在 GitHub release 啦。

我将修改后的版本传到了 npm，包名为 `changelogen-gh`。这样就可以很方便的集成到 GitHub Action 中了。

**示例：**

```yml
name: Release Changelog

on:
  push:
    tags:
      - 'v*'

permissions:
  contents: write

jobs:
  changelog:
    runs-on: ubuntu-latest
    steps:
      - name: Checkout code
        uses: actions/checkout@v4
        with:
          fetch-depth: 0
          ref: main

      - name: Setup git config
        run: |
          git config user.name "github-actions[bot]"
          git config user.email "41898282+github-actions[bot]@users.noreply.github.com"

      - name: Release changelog
        run: npx changelogen-gh --release --push
        env:
          GITHUB_TOKEN: ${{secrets.GITHUB_TOKEN}}
```

以上 Action 做了这些事情：
1. 设置好仓库。
2. 根据当前 tag 以及上一条 tag 生成或更新 (追加内容) `CHANGELOG.md`。
3. 将修改后的文件提交 commit 并推送到仓库。
4. 创建或更新 GitHub Release。

## 不足

既然删除了自动打 tag 功能，也就意味着我们需要自己手动打 tag。这有利有弊吧，不过需要严格遵守[语义化版本](https://semver.org)。并且包管理器 (`Cargo.toml`, `package.json`, etc) 中的版本也不能忘记。

并且，Changelog 的生成是根据 [conventional commit](https://www.conventionalcommits.org) 生成的， 如果撰写 commit 不规范，生成的 changelog 也质量不高。

[keepachangelog](https://keepachangelog.com) 中也指出，
> Changelogs are for humans, not machines. 

用工具生成的 changelog 难免有违「人」，所以在大的开源项目中，也需谨慎使用。