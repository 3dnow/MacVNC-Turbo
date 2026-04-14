🍏 MacVNC-Turbo
📖 Description / 项目简介
MacVNC-Turbo is a brutally minimalist, zero-dependency solution for remote controlling a Mac from Windows.
Its biggest feature: A tiny, standalone WebSocket proxy. You do not need to install any heavy third-party remote control software, clients, or runtime libraries on either machine. Just run the lightweight proxy executable, open the HTML file in your browser, and you get instant, ultra-low latency, native access to your macOS desktop.
MacVNC-Turbo 是一个极致精简、零依赖的跨平台 Mac 远程控制解决方案。
我们最大的特点： 核心仅包含一个极小的 WebSocket 代理程序。你完全不需要在两端安装任何臃肿的第三方远程软件、客户端或运行库。只需运行这个轻量级的代理，用浏览器打开一个网页，就可以直接获得超低延迟的 Mac 桌面控制权！极客、纯净、即插即用。
✨ Features / 核心特性
🪶 Zero Dependencies & Browser-Based: No installations required. Connect directly via Chrome or Edge using a single HTML file.
(零依赖 & 纯浏览器控制：无需安装任何客户端，一个 HTML 文件直接在 Edge/Chrome 浏览器中完成控制。)
🚀 Tiny Ultra-Low Latency Proxy: A micro C++ native Windows proxy (WebSockify) that translates VNC frames at nanosecond speed.
(极小体积的低延迟代理：原生的 C++ 微型代理程序，纳秒级转发 VNC 数据包，榨干局域网极限。)
💻 Protocol-Level Modifier Swapping: Seamlessly use your Windows Left Ctrl key as the Mac Cmd key (e.g., Ctrl+C, Ctrl+V, Ctrl+A). Implemented via internal RFB protocol monkey-patching—zero double-typing bugs! Use Right Ctrl to send a real Ctrl signal (e.g., Right Ctrl+C for SIGINT in a terminal).
(底层协议级按键映射：左 Ctrl+C/V/A 自动转换为 Mac 的 Cmd 指令；右 Ctrl 原样发送真实 Ctrl 信号（如在终端中发送 Ctrl+C 中断信号）。彻底消灭双击/抢跑 Bug。)
🏎️ Overdrive Mode: Force macOS to stop applying heavy compression algorithms and let your Windows GPU take over the 3D rendering workload.
(Overdrive 性能全开模式：强制接管浏览器 GPU 3D 渲染，解放高分辨率（Retina）下的卡顿灾难。)
🪟 Pixel-Perfect 1:1 Display: Easily disable scaling to enjoy perfectly crisp, unscaled retina display text.
(像素级原画显示：支持关闭浏览器缩放，实现 1:1 绝对物理像素点对点显示，字体锐利清晰。)
🍏 Apple Auth Support: Automatically handles macOS's proprietary VNC authentication (requiring both username and password).
(完美兼容 Apple Auth 协议：自动处理 macOS 专属的用户名+密码二次安全校验，告别连接卡死。)
📋 Clipboard Bridge (clipboard_bridge.py): Bidirectional clipboard sync via a separate WebSocket channel, completely bypassing Apple VNC's broken clipboard support. Pure Python, zero external dependencies. Supports full Unicode including Chinese.
(剪切板双向同步桥接：通过独立 WebSocket 通道绑过 Apple VNC 协议层的剪切板缺陷，实现真正的双向剪切板同步。纯 Python 实现，无需安装任何第三方库，完整支持中文等 Unicode 内容。)
🛠️ Usage / 如何使用
Step 1: Run the Proxy / 运行底层代理 (Windows)
Run the compiled proxy executable, pointing it to your Mac's IP address and a local port of your choice (e.g., 6080).
(直接运行代理程序，指向你 Mac 在局域网的 IP 地址即可打通隧道：)
WebsocketTcp.exe <Your_Mac_IP> 6080

Step 2: Connect via the Web Client / 网页端一键连接
Double click vncmac.html to open it in Chrome or Edge.
(双击 vncmac.html 在浏览器中打开。)
Ensure the Proxy URL matches your local port (default: ws://127.0.0.1:6080).
(确认界面上的代理端口与运行的一致。)
Click Connect. When prompted, enter your Mac Login Username and Password.
(点击连接，根据弹窗提示输入 Mac 的登录账号与密码。)
Enjoy a flawless remote desktop experience!
(享受丝滑的远程控制体验！)

📋 Clipboard Bridge Setup / 剪切板桥接配置
Apple's built-in VNC (Screen Sharing) does not support clipboard sync over the standard VNC protocol. The Clipboard Bridge is a lightweight Python WebSocket server that runs on the Mac and provides true bidirectional clipboard sync.
(Apple 自带 VNC（屏幕共享）不支持标准 VNC 剪切板协议。剪切板桥接是一个运行在 Mac 上的轻量级 Python WebSocket 服务器，提供真正的双向剪切板同步。)
Step 1: Start the bridge on Mac
(在 Mac 上启动桥接程序)
    # No token (LAN/localhost only)
    python3 clipboard_bridge.py

    # With token (recommended when exposing to network)
    python3 clipboard_bridge.py 9001 your-secret-token

    # Default port is 9001. Listens on 0.0.0.0 (all interfaces).

Step 2: Connect from the browser
(从浏览器连接)
In the top bar, find the "📋 Bridge:" input. Enter the WebSocket URL and click Connect:
(在顶栏找到 "📋 Bridge:" 输入框，填入 WebSocket 地址并点击 Connect：)
    ws://127.0.0.1:9001               (localhost / SSH tunnel)
    ws://192.168.1.100:9001           (LAN direct)
    ws://192.168.1.100:9001?token=xxx (with token auth)

The dot turns 🟢 when connected. If it drops unexpectedly, an alert will appear.
(连接成功后指示灯变为 🟢。如果意外断开，会弹出告警提示。)
Step 3: Use clipboard buttons
(使用剪切板按钮)
    📋 →Mac : Copies browser clipboard → sets Mac clipboard. Then press Cmd+V on Mac.
               (将浏览器剪切板内容推送到 Mac，然后在 Mac 上按 Cmd+V 粘贴。)

    📋 Mac→ : Reads Mac clipboard → writes to browser clipboard.
               (读取 Mac 剪切板内容，同步到浏览器剪切板。)

Note: Both buttons require the Bridge to be connected. Without the Bridge, clipboard sync is unavailable (Apple's VNC limitation).
(注意：两个按钮均需 Bridge 已连接。没有 Bridge 时，剪切板同步不可用，这是 Apple VNC 协议层面的限制。)
SSH Tunnel (remote access) / SSH 隧道（远程访问）
If accessing the Mac from outside the LAN, forward port 9001 alongside the VNC port:
(如果从局域网外访问 Mac，需同时转发 9001 端口和 VNC 端口：)
    ssh -L 6800:localhost:6800 -L 9001:localhost:9001 user@your-mac-ip

Then use ws://127.0.0.1:9001 in the browser as usual.
(然后在浏览器中照常使用 ws://127.0.0.1:9001。)
💡 Tips for the Best Experience / 终极体验优化指南
If you have a Retina Mac (4K/5K) and experience lag over Wi-Fi:
(如果你控制的是 4K/5K 分辨率的 Mac 且感觉不够跟手：)
Check the 🚀 Overdrive Mode in the top bar to force maximum framerate.
Uncheck 🪟 Scale to disable JavaScript scaling lag.
Crucial: Change your Mac's display resolution to 1920x1080 (HiDPI) during remote sessions. Standard VNC protocol struggles to software-encode 4K pixels quickly enough.
🙏 Credits / 致谢声明
This project's web frontend utilizes the core RFB implementation from noVNC. We extend our gratitude to the noVNC team for their incredible open-source contribution to browser-based VNC clients.
(本项目的网页前端核心 RFB 协议解析依赖于优秀的开源项目 noVNC。特此向 noVNC 团队在浏览器 VNC 客户端领域做出的卓越贡献表示感谢！)
📜 License
MIT License
Copyright (c) 2024
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
