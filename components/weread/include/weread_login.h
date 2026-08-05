// weread_login.h — 微信读书扫码登录（复刻 weread_qr_login.py 流程）
// 纯 ESP-IDF：std::string 替代 Arduino String，cJSON 替代 ArduinoJson。
#pragma once
#include <string>
#include <functional>

namespace weread_login {

// 扫码登录主流程（阻塞，直到成功/失败/超时）。成功返回 true 且 cookie 已写入配置文件。
// render_qr(url, status): 由上层提供，把 url 画成二维码并显示状态文字
// msg(l1,l2,l3): 由上层提供，显示 3 行消息
bool run(std::function<void(const std::string& url, const std::string& status)> render_qr,
         std::function<void(const std::string&, const std::string&, const std::string&)> msg,
         unsigned long timeout_ms = 240000);

} // namespace weread_login
