# 第三方与素材说明

- LeviLamina：https://github.com/LiteLDev/LeviLamina 。本项目通过普通26.60.7 SDK构建；实验分支 feat/custom-server-readme 仅用于机制参考，不替换或重新分发该分支加载器。LeviLamina 自身许可见上游 COPYING / COPYING.LESSER。
- Dear-OreUI：https://github.com/copper-lamp/Dear-OreUI ，上游源码采用 CC0-1.0。历史桥接代码仅供研究，不参与默认构建；third_party 参考仓库已取消跟踪并列入忽略名单，不随源码提交或安装包分发。当前原生图片、缓存与UI运行不依赖 DearOreUI.dll。
- nlohmann/json：MIT，Windows 使用 xmake 的 v3.12.0，Android 使用官方模板兼容的 v3.11.3；许可见上游 https://github.com/nlohmann/json 。Android 包另附对应 JSON-LICENSE.txt。
- Android 版依据 [官方教程](https://levimc.org/zh-Hans/tutorials/develop/first-android-mod/) 和 [Preloader SDK：LeviLauncher 1.6.0 对应提交](https://github.com/LiteLDev/preloader-android/tree/5d108da350ce3e7f13f51415f81e65344ecd5d14) 的公开接口实现；不分发启动器运行库。fmt 11.2.0（MIT）静态链接，Android 包附 FMT-LICENSE.txt；Boost.PFR 1.88.0（Boost Software License 1.0）与 magic_enum 0.9.7（MIT）用于配置/schema 生成及配置反射，Android 包附 BOOST-LICENSE.txt、MAGIC-ENUM-LICENSE.txt，未分发独立运行库。
- 草方块默认Logo和风景默认简介图：本项目原创的可重复生成像素画，不抽取Minecraft贴图。原始PNG及生成器随源码保留。
- 原版按钮贴图运行时从玩家已安装游戏加载；本安装包不分发游戏贴图、字体、商标或其它Mojang资源。Minecraft相关权利属于其各自权利人。

## nlohmann/json v3.12.0 — MIT License

Copyright (c) 2013-2025 Niels Lohmann

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
