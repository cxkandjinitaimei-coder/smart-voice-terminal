"""Build evidence, independent history snapshot and Hub handoff for timelines."""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
import zlib

ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'AGENTS.md').is_file())
HERE = ROOT / "flask_relay"
EVIDENCE = HERE / "timeline_validation"
HUB = Path("D:/AI-Agents-Hub")
TASK = "[ArdProject] 一次下发多动作时间序列、表情轮播、可调点头摇头限位并同步AI/MCU"


def claim():
    stamp = datetime.now().strftime("%Y-%m-%d %H:%M")
    state = json.loads((HUB / "state.json").read_text(encoding="utf-8-sig"))
    project = state["projects"]["ArdProject"]
    print(json.dumps({k: project.get(k) for k in ["root", "services_status"]}, ensure_ascii=False))
    todo = HUB / "todo.md"
    with todo.open("a", encoding="utf-8") as f:
        f.write("\n- [ ] " + TASK + "；claimed by Codex " + stamp + "\n")
    with (HUB / "worklog.md").open("a", encoding="utf-8") as f:
        f.write(stamp + "  Codex | " + TASK + " 开始，继承03音频修复与旧历史包。\n")
    project["services_status"] = "时间序列功能开发中；未停止旧网页、未烧板或部署公网。"
    (HUB / "state.json").write_text(json.dumps(state, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
    EVIDENCE.mkdir(exist_ok=True)
    shutil.copy2(ROOT / "HANDOFF.md", EVIDENCE / "上轮HANDOFF.md")


def stage():
    version = re.search(r'APP_FW_VERSION "([^"]+)"', (ROOT / "ST32/AIAssistant/User/app_version.h").read_text()).group(1)
    index = HERE / "templates/index.html"
    index.write_text(re.sub(r'W2026\.10\.08-03', 'W'+version[1:], index.read_text(encoding="utf-8")), encoding="utf-8")
    lan = EVIDENCE / "lan_src/sketch_aug28a"
    lan.mkdir(parents=True, exist_ok=True)
    for p in (ROOT / "sketch_aug28a").iterdir():
        if p.suffix.lower() in [".ino", ".h", ".cpp"]:
            shutil.copy2(p, lan / p.name)
    p = lan / "sketch_aug28a.ino"
    source = p.read_text(encoding="utf-8-sig").replace('#include "firmware_version.h"', '#include "firmware_version.h"\n#include "lan_config.h"')
    source, count = re.subn(r'const char\* SERVER_IPS\[\] = \{.*?\};', 'const char* SERVER_IPS[] = { LAN_SERVER_IP };', source, count=1, flags=re.S)
    assert count == 1
    p.write_text(source, encoding="utf-8")
    shutil.copy2(ROOT / "交付/20261008/20261008_03_录音Flash后台写入与串口重传/ESP局域网源码/sketch_aug28a/lan_config.h", lan / "lan_config.h")
    p = lan / "firmware_version.h"
    p.write_text(p.read_text().replace('"'+version+'"', '"'+version+'-LAN"'), encoding="ascii")
    print("Staged pure LAN " + version)


def finish():
    from package_decoder_fix import FILES, verify, seal
    version = re.search(r'APP_FW_VERSION "([^"]+)"', (ROOT / "ST32/AIAssistant/User/app_version.h").read_text()).group(1)
    tag = version[1:].replace(".", "").replace("-", "_")
    out = ROOT / "交付" / tag[:8] / (tag+"_动作时间序列与可调舵机限位")
    assert out.exists()
    lanbin = (EVIDENCE / "esp_lan_build/sketch_aug28a.ino.bin").read_bytes()
    assert (version+"-LAN").encode() in lanbin
    shutil.copytree(EVIDENCE / "lan_src/sketch_aug28a", out / "ESP局域网源码/sketch_aug28a")
    shutil.copy2(EVIDENCE / "esp_lan_build/sketch_aug28a.ino.bin", out / ("firmware/ESP8266_LAN_"+tag+".bin"))
    web = out / "网页局域网"
    web.mkdir()
    for name in FILES + ["motion_timeline.py", "lan_start.py", "start_lan.ps1", "一键启动局域网.bat", "allow_lan_firewall.ps1", "允许局域网访问.bat"]:
        if name not in ["deploy_web.ps1", "一键更新网页到云端.bat", "一键更新网页到云端.ps1", "README.md"]:
            shutil.copy2(HERE / name, web / name)
    for folder in ["static", "templates"]:
        shutil.copytree(HERE / folder, web / folder)
    (web / "lan_seed.json").write_text(json.dumps({"source_config": "../../../20261008/20261008_03_录音Flash后台写入与串口重传/网页局域网/config.json",
        "fallback_configs": ["../../../20261008/20261008_02_局域网语音识别与OTA/网页局域网/config.json"]}, ensure_ascii=False)+"\n", encoding="utf-8")
    # Existing 03 Keil snapshot contains project support files; overlay all current sources.
    previous = ROOT / "交付/20261008/20261008_03_录音Flash后台写入与串口重传"
    verify(previous)
    shutil.copytree(previous / "F407源码", out / "F407源码")
    for folder in ["App", "Hardware", "System", "Core", "Lib", "User"]:
        for p in (ROOT / "ST32/AIAssistant" / folder).rglob("*"):
            if p.is_file() and p.suffix.lower() in [".c", ".h", ".s"] and not any(part.startswith(("Objects", "Listings")) for part in p.parts):
                dest = out / "F407源码" / p.relative_to(ROOT / "ST32/AIAssistant")
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(p, dest)
    shutil.copy2(ROOT / "ST32/AIAssistant/User/MyAIMachines_OTA.uvprojx", out / "F407源码/User/MyAIMachines_OTA.uvprojx")
    validation = out / "验证"
    validation.mkdir()
    for p in EVIDENCE.glob("*.log"):
        shutil.copy2(p, validation / p.name)
    shutil.copy2(EVIDENCE / "时间序列网页.png", validation / "时间序列网页.png")
    shutil.copytree(ROOT / "ST32/AIAssistant/Tools/timeline_test", out / "F407源码/Tools/timeline_test",
                    ignore=shutil.ignore_patterns("__pycache__"))
    for name in ["harness.c", "run_tests.py"]:
        shutil.copy2(ROOT / "ST32/AIAssistant/Tools/esp_link_test" / name, out / "F407源码/Tools/esp_link_test" / name)
    for name in ["test_motion_timeline.py", "test_timeline_browser.py"]:
        shutil.copy2(HERE / name, validation / name)
    metadata = json.loads((out / "release.json").read_text())
    metadata.update(web_version="W"+version[1:], esp_lan_build=version+"-LAN", esp_lan_bytes=len(lanbin),
                    esp_lan_crc32="%08X" % zlib.crc32(lanbin), update_boards=["F407", "ESP8266", "WEB"],
                    hardware_verified=False, max_timeline_steps=32, max_timeline_total_ms=120000)
    (out / "release.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
    seal(out)
    print(str(out)+" SHA entries="+str(verify(out)))


def finalize():
    from package_decoder_fix import verify, seal
    version = re.search(r'APP_FW_VERSION "([^"]+)"', (ROOT / "ST32/AIAssistant/User/app_version.h").read_text()).group(1)
    tag = version[1:].replace(".", "").replace("-", "_")
    out = ROOT / "交付" / tag[:8] / (tag+"_动作时间序列与可调舵机限位")
    assert out.is_dir()
    public = EVIDENCE / "esp_public_build/sketch_aug28a.ino.bin"
    lan = EVIDENCE / "esp_lan_build/sketch_aug28a.ino.bin"
    assert public.stat().st_mtime >= (ROOT / "sketch_aug28a/sketch_aug28a.ino").stat().st_mtime
    assert lan.stat().st_mtime >= (EVIDENCE / "lan_src/sketch_aug28a/sketch_aug28a.ino").stat().st_mtime
    for source, dest in [(public, out/("firmware/ESP8266_"+tag+".bin")),
                         (lan, out/("firmware/ESP8266_LAN_"+tag+".bin"))]: shutil.copy2(source,dest)
    for source, dest in [(ROOT / "sketch_aug28a",out / "ESP源码/sketch_aug28a"),
                         (EVIDENCE / "lan_src/sketch_aug28a",out / "ESP局域网源码/sketch_aug28a")]:
        for p in source.iterdir():
            if p.suffix.lower() in [".ino", ".h", ".cpp"]: shutil.copy2(p,dest/p.name)
    for folder in ["网页局域网", "网页一键更新"]:
        for name in ["app.py", "ai_relay.py", "device_profile.py", "motion_timeline.py", "lan_start.py"]:
            if (out / folder / name).is_file(): shutil.copy2(HERE/name,out/folder/name)
        for name in ["app.js", "page-interact.js", "page-timeline.js"]:
            shutil.copy2(HERE / "static" / name,out/folder/"static"/name)
    for p in EVIDENCE.glob("*.log"): shutil.copy2(p,out/"验证"/p.name)
    for p in (ROOT / "ST32/AIAssistant/Tools/timeline_test").iterdir():
        if p.is_file(): shutil.copy2(p,out/"F407源码/Tools/timeline_test"/p.name)
    metadata=json.loads((out/"release.json").read_text())
    metadata.update(esp_bytes=public.stat().st_size, esp_lan_bytes=lan.stat().st_size,
                    esp_lan_crc32="%08X" % zlib.crc32(lan.read_bytes()))
    (out/"release.json").write_text(json.dumps(metadata,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    stamp=datetime.now().strftime("%Y-%m-%d %H:%M")
    handoff=f"""最近更新: {stamp}
最后操作者: Codex

## 本次做了什么

- 一次下发的动作时间序列闭环：AI/网页timeline严格校验与手势展开 → ESP单帧二进制0x20 → F407非阻塞执行。32步、同时间多轴+表情、循环1~20次、计划总长≤120秒；新任务替换、停止、手动动作打断、录音/字体/OTA冻结、重复帧不重启。表情绘制屏障避免只剩最后一张，BMP逐行服务PWM。
- 用户追加的舵机限位：管理员pitch/yaw min/max/front同步0x22并回执CRC，AI下一轮使用新范围，F407立即/手势/扫动/时间表遵守限位。调整限位终止旧计划。首次PWM不从虚拟0°缓动，上电三轴默认90°。管理员配置在网页保存，板子启动需ESP重新同步；不是MCU断电持久化。
- 网页测试页生成/检查/整段发送/停止、表情轮播/点头/摇头/组合范例、硬件执行进度与限位确认；AI仍能自由组合角度/速度/时间，不是固定动作集。
- 真Edge管理员编辑与编排验收通过，真实MCU调度/串口分发、ESP编码/实际转发函数、PWM、音频实际传输/协议/队列/故障状态、OTA、LAN启动、部署和对话投递共15套回归通过。Keil全量0E0W；两套Arduino实编。
- 交付/20261008/20261008_04_动作时间序列与可调舵机限位：F407 {version} {metadata['app_bytes']}B，rawCRC {metadata['app_crc32']}，整Slot A CRC {metadata['full_slot_a_crc32']}；ESP1.1.7公网与LAN；网页W2026.10.08-04。完整Keil快照与活工程同步，旧03及此前历史未改。没有烧板/发动作/部署公网/重启旧服务，实板待用户。

## 改了哪些文件

- ST32/AIAssistant/App/motion_timeline.h、ailink.c/h、servo_motion.h、media.c；User/main.c、app_version.h；Tools/release_audio.py、timeline_test/*、esp_link_test/harness.c、servo_pwm_test/harness.c。
- sketch_aug28a/bridge_motion.h、sketch_aug28a.ino、firmware_version.h。其余03音频Flash后台缓冲/重传代码保留。
- flask_relay/motion_timeline.py、app.py、ai_relay.py、device_profile.py、lan_start.py、deploy_web.ps1、static/app.js/page-test.js/page-timeline.js/page-interact.js/page-settings.js、templates/index.html。
- flask_relay/test_motion_timeline.py/test_timeline_browser.py/test_device_profile.py/test_deploy_web.py、run_timeline_regression.py、timeline_release.py、timeline_validation/；新04包验收说明/固件/完整源/两个网页入口/SHA与证据。

## 遗留问题与坑

- 新04实际屏幕/舵机待用户上板验收，程序回执是PWM目标完成，没有轴位置反馈。SD绘制与运动收尾会延长实际时间，录音/OTA暂停不计计划时间，重启清空任务。FACE图片仍必须放SD正确8.3文件名。修改限位不自动切舵机电源。
- 前轮03音频0.64秒故障修的是后台Flash/串口重传缺陷，仍未实板确认；04继承，不能宣称唯一实板根因或已修好录音。当前旧网页/板子版本未变，新包需三端配套更新。
- F407继续Slot A=0x08008000、K-UP确认OTA；不要SWD单独覆盖Slot A导致旧CRC元数据停Boot。不改Bootloader/字库/引脚。公网<SERVER_IP>仍未部署本轮；Linuxsmall已MODEL_READY，PyAV18.1.0修复继承，不重新下载。
- LAN新包优先继承03参数，再02，再根配置；旧录音留旧目录，新包队列独立。本机ASR模型复用，LAN网址以启动窗口实时IP为准。<=1秒音频上传尾延迟的受控回归不代表公网保证。
- 看门狗未接入。git大量此前dirty/untracked，未reset/clean/全stage或提交。前轮详细交接归档在flask_relay/timeline_validation/上轮HANDOFF.md。

## 给下一个 AI 的交接点

1. 用户按新04验收说明：先当前网页K-UP OTA firmware/F407_OTA_20261008_04.bin，再Arduino上传ESP局域网源码整套，最后关闭旧网页窗口、启动网页局域网/一键启动局域网.bat。屏{version}、ESP1.1.7、网页W04，限位必须显示已同步MCU。
2. 测试表情2秒轮播3次，点头/摇头与停止；管理员改pitch min82/max98/front92，确认生成时间表与AI范围更新。timeline.steps[]的move支持servo/expression/gesture，手势按当前档案展开；动作时间从F407接收完成计。
3. python flask_relay/run_timeline_regression.py 跑15套；python ST32/AIAssistant/Tools/timeline_test/run_tests.py；全量Keil用D:/AAkeil/UV4/UV4.exe -r User/MyAIMachines_OTA.uvprojx、fromelf输出bin；ArduinoCLI nodemcuv2。不要跑真实AI费用/实设备命令代替受控回归。
4. 后续新固件继续release_audio.py prepare按用户日期→实际编译→package独立历史目录，04不覆盖。03旧历史包SHA保留。包内release.json、验证日志与SHA为验收依据。
"""
    for path in [ROOT / "HANDOFF.md",ROOT / "ST32/AIAssistant/HANDOFF.md"]:
        path.write_text(handoff,encoding="utf-8")
    (out/"HANDOFF.md").write_text(handoff,encoding="utf-8")
    todo=HUB/"todo.md"
    text=todo.read_text(encoding="utf-8-sig").replace("- [ ] "+TASK,"- [x] "+TASK)
    if "[ArdProject] V2026.10.08-04实板：" not in text:
        text+="\n- [ ] [ArdProject] V2026.10.08-04实板：三端更新、限位CRC确认、2秒表情轮播3次、点头摇头与停止/暂停；软件交付已完成。 "+stamp+"\n"
    todo.write_text(text,encoding="utf-8")
    state=json.loads((HUB/"state.json").read_text(encoding="utf-8-sig"))
    project=state["projects"]["ArdProject"]
    project.setdefault("completed",[]).append("2026-10-08 V04动作时间序列与管理员点头/摇头限位：三端代码、14套回归、两端真实构建、独立历史包完成；未烧板。")
    issue="V2026.10.08-04屏幕/舵机/限位同步实板待验收；配置在网页保存，上电需ESP同步；无轴反馈，看门狗未接入，03音频故障实板待复测。"
    if isinstance(project.get("known_issues"),list): project["known_issues"].append(issue)
    else: project["known_issues"]=str(project.get("known_issues", ""))+"；"+issue
    project["services_status"]="V04独立软件交付完成；旧网页/板子/公网未改。用户需更新F407、ESP、WEB，硬件待验收。"
    (HUB/"state.json").write_text(json.dumps(state,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    with (HUB/"worklog.md").open("a",encoding="utf-8") as f:
        f.write(stamp+"  Codex | "+TASK+" 完成V2026.10.08-04独立包，15套回归/Keil0E0W/Arduino双版本通过；旧包保留，硬件待验收。\n")
    seal(out)
    count=verify(out)
    for name in ["20261008_03_录音Flash后台写入与串口重传", "20261008_02_局域网语音识别与OTA", "20261007_06_舵机PWM慢速平滑"]:
        print(name+" historical SHA entries="+str(verify(ROOT/"交付"/name[:8] / name)))
    print(version+" FINAL "+str(out)+" SHA entries="+str(count))


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    ap=argparse.ArgumentParser()
    ap.add_argument("mode", choices=["claim", "stage", "finish", "finalize"])
    globals()[ap.parse_args().mode]()
