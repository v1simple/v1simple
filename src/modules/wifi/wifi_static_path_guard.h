#pragma once

namespace WifiStaticPathGuard {

bool isSafe(const char* path);
bool isAllowedServedPath(const char* path);
bool isHtmlPagePath(const char* path);

} // namespace WifiStaticPathGuard
