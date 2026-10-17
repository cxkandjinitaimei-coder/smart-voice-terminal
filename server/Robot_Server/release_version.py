"""release_version.py：网页及识别模块的公开版本常量。

使用：从根 VERSION 经 tools/set_version.py 生成，模板由 app 的 context_processor 注入。
版本用于展示和静态资源缓存，不包含产品名、私有服务地址或设备身份。
"""
APP_VERSION = "1.0.0"
