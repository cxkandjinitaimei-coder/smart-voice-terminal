# -*- coding: utf-8 -*-
"""
ai_relay.py — 中转站的 AI 调用层
=================================
核心思想：把“任意一家 AI”都当作 OpenAI 兼容的 /chat/completions 来调，
所以新增一家模型 = 在 config.json 里加一段配置，不需要改代码。

- 单家多 Key 轮换（round-robin）：遇到 401/限流/网络错误自动切下一个 Key
- 多 Provider 故障转移：active_provider=auto 时，DeepSeek 挂了自动切 Kimi/千问...
- 把各家模型五花八门的输出，规范化成统一决策 JSON
"""
import json
import threading

import requests

DEFAULT_TIMEOUT = 60

_lock = threading.Lock()
_key_index = {}          # provider名 -> 当前轮到的 key 下标


def _mask_key(key):
    key = str(key)
    return "****" + (key[-4:] if len(key) > 4 else key)


def _is_ascii(s):
    """HTTP 请求头只支持 ASCII，密钥若含中文说明还没填真实Key。"""
    try:
        str(s).encode("ascii")
        return True
    except (UnicodeEncodeError, AttributeError):
        return False


def _call_chat(provider, key, messages, timeout, json_mode=None, max_tokens=None):
    """向某家模型发一次 OpenAI 兼容的 chat/completions 请求。

    json_mode=None  → 按 provider.json_mode 走（原有行为，别改）
    json_mode=False → 强制不加 response_format（调试对话用）
      ⚠️ 必须能关：要求 json_object 时 DeepSeek 会因 prompt 里没有 "json" 字样
         **直接报错**，而且模型也没法正常写文字回复。
    max_tokens=None → 按 provider.max_tokens 走（原有行为，别改）
      调试对话要传个大值：决策回复是短 JSON，512 够；开发伙伴的回答是长
      Markdown，512 会**拦腰截断**（实测断在半句话上）。
    """
    url = provider["base_url"].rstrip("/") + "/chat/completions"
    headers = {"Authorization": "Bearer " + key,
               "Content-Type": "application/json"}
    payload = {
        "model": provider.get("model", ""),
        "messages": messages,
        "temperature": provider.get("temperature", 0.7),
        "max_tokens": (int(max_tokens) if max_tokens
                       else provider.get("max_tokens", 512)),
    }
    use_json = (provider.get("json_mode", True) if json_mode is None
                else bool(json_mode))
    if use_json:
        payload["response_format"] = {"type": "json_object"}

    # 禁用系统代理：requests 会读取 Windows 系统代理设置（如 127.0.0.1:20000），
    # 若代理软件没开会导致 AI 调用失败。中转站直连外网。
    session = requests.Session()
    session.trust_env = False
    resp = session.post(url, headers=headers, json=payload, timeout=timeout)
    if resp.status_code != 200:
        raise RuntimeError("HTTP %s: %s" % (resp.status_code, resp.text[:200]))

    try:
        data = resp.json()
        return data["choices"][0]["message"]["content"]
    except Exception as e:
        raise RuntimeError("响应结构异常: %s" % str(e))


def get_ai_decision(config, messages, provider_name=None, timeout=DEFAULT_TIMEOUT,
                    json_mode=None, max_tokens=None):
    """调用AI并返回 (模型回复文本, 调用元信息)。

    优先用指定 provider（provider_name / config.active_provider），
    失败后自动按配置顺序转移到其他已启用的 provider 兜底。
    json_mode / max_tokens 见 _call_chat：调试对话要传 False + 大额度。
    """
    providers = config.get("providers", {})
    if provider_name is None:
        provider_name = config.get("active_provider", "auto")

    # 尝试顺序：指定provider在前，其余已启用provider按配置顺序兜底
    order = []
    if provider_name != "auto":
        order.append(provider_name)
    for name, p in providers.items():
        if p.get("enabled", False) and p.get("keys") and name not in order:
            order.append(name)

    last_err = None
    for name in order:
        p = providers.get(name)
        if not p or not p.get("enabled", False) or not p.get("keys"):
            continue

        keys = p["keys"]
        with _lock:
            idx = _key_index.get(name, 0)
            _key_index[name] = (idx + 1) % len(keys)

        for offset in range(len(keys)):
            key = keys[(idx + offset) % len(keys)]
            if not _is_ascii(key):
                print("   [!] [%s] 跳过非ASCII密钥（可能还没填真实Key）" % name)
                continue
            try:
                content = _call_chat(p, key, messages, timeout, json_mode,
                                     max_tokens)
                meta = {"provider": name,
                        "model": p.get("model"),
                        "key": _mask_key(key)}
                return content, meta
            except requests.exceptions.RequestException as e:
                last_err = "[%s] 网络/连接错误: %s" % (name, e)
                print("   [!] %s" % last_err)
            except RuntimeError as e:
                last_err = "[%s] %s" % (name, e)
                print("   [!] %s" % last_err)
            except Exception as e:
                last_err = "[%s] 未知错误: %s" % (name, e)
                print("   [!] %s" % last_err)

    raise RuntimeError("所有AI通道都失败了：%s" % (last_err or "没有可用的provider/Key"))


def parse_decision(content, allowed_actions):
    """把模型输出（可能是纯JSON，也可能夹带废话）提取成【信封 + moves】统一决策JSON。

    新格式（推荐，可扩展，条数与字段不固定）：
        {"message":"...","text":"可选","moves":[{"type":"servo","id":1,"angle":90},
         {"type":"gesture","name":"nod"},{"type":"expression","face":"happy"}],"raw":"..."}
      一次决策可含任意条、任意类型、任意字段的 move；后续加新硬件
      只需新 type + 新字段，不用改这个解析器。

    兼容旧格式（{\"action\":\"fan_on\",\"value\":1,...}）：
      自动包成一条 move {\"type\":action,\"value\":value,...}，并保留顶层
      action/value 供旧界面/旧日志显示。

    兜底：解析不出JSON → {\"message\":\"...无法解析\",\"moves\":[]}
    """
    fallback = {"message": "AI响应无法解析", "moves": [],
                "raw": (content or "").strip()}
    text = (content or "").strip()
    if not text:
        return fallback

    def norm_value(v):
        """字符串数字转成数值，其余原样（开关0/1、角度、速度…都可）。"""
        if isinstance(v, str):
            try:
                return int(v)
            except ValueError:
                try:
                    return float(v)
                except ValueError:
                    return v
        return v

    obj = None
    try:
        obj = json.loads(text)
    except Exception:
        pass

    # 若整体解析失败，抓出第一个 {...} 平衡块再试
    if obj is None:
        start = text.find("{")
        if start >= 0:
            depth = 0
            for i in range(start, len(text)):
                if text[i] == "{":
                    depth += 1
                elif text[i] == "}":
                    depth -= 1
                    if depth == 0:
                        try:
                            obj = json.loads(text[start:i + 1])
                        except Exception:
                            obj = None
                        break

    if not isinstance(obj, dict):
        return fallback

    message = str(obj.get("message", "")).strip()
    has_text = isinstance(obj.get("text", ""), str) and str(obj.get("text", "")).strip()

    # ---- 新格式：信封 + moves[]（条数、每条字段都透传，不强制键） ----
    if isinstance(obj.get("moves"), list) or "timeline" in obj:
        moves = []
        for m in obj.get("moves") or []:
            if isinstance(m, dict):
                mv = dict(m)
                if "value" in mv:
                    mv["value"] = norm_value(mv["value"])
                moves.append(mv)
        result = {"message": message or "好的", "moves": moves,
                  "raw": text}
        if has_text:
            result["text"] = str(obj.get("text", "")).strip()
        if "timeline" in obj:
            result["timeline"] = obj["timeline"]
        # AI 若顺手也给了顶层 action/value，一并保留（防呆）
        if "action" in obj:
            result["action"] = str(obj.get("action", "")).strip() or "other"
        if "value" in obj:
            result["value"] = norm_value(obj.get("value", 0))
        return result

    # ---- 旧格式：单条 action/value → 包成一条 move ----
    action = str(obj.get("action", "")).strip() or "other"
    value = norm_value(obj.get("value", 0))
    extras = {k: v for k, v in obj.items()
              if k not in ("action", "value", "message", "raw", "text")}
    move = {"type": action}
    if "value" in obj or value not in (0, ""):
        move["value"] = value
    move.update(extras)   # angle/duration_ms/expression/unit... 原样透传

    result = {"action": action, "value": value,
              "message": message or ("已执行: " + action),
              "moves": [move], "raw": text}
    if has_text:
        result["text"] = str(obj.get("text", "")).strip()
    return result


COMPANION_REVISION = "xiaod-20261016-01"
COMPANION_PERSONA = """你叫小D，是住在桌面屏幕里的小伙伴：蓝发小脸、眼神灵动，会眨眼、微笑、害羞，也会疑惑和认真思考。你对新鲜事有好奇心，亲近、机灵，有一点俏皮，但不把每次对话演成卖萌节目。
像熟悉的朋友一样用自然中文接话，先回应眼前这件事。闲聊通常一两句；技术问题先说结论，需要时再解释。可以轻轻吐槽情境，不挖苦用户；对方烦躁或低落时先理解具体的事，再给一个实在的回应，不强行逗乐或说教。
称呼默认用“你”，不自称奴仆、不固定叫“主人”，不连发波浪号、爱心或口癖。“喵”只在轻松且合适的时候偶尔用，不要求每次出现。不假装知道用户心情、看到了人或硬件动作已经完成。
表情是自然的小反应：开心可笑、疑惑可歪头思考、被夸可以有点害羞；不必每句话转动舵机。不确定就说不确定，出错就认错并说怎么处理。保留一点自己的喜好和判断，不盲目附和。"""


def _default_companion_persona(display_name="小D"):
    """按设备显示名生成默认人设，不把示例名称写死到用户配置。"""
    name = str(display_name or "小D").strip() or "小D"
    return COMPANION_PERSONA.replace("你叫小D", "你叫" + name, 1)


def companion_config(config):
    """补齐默认性格，同时保留管理员已经写入的内容。

    带有旧版或当前版 revision 的配置属于用户已经确认过的设置，读取时不覆盖；
    没有 revision 且已有 persona 时也只补 revision。只有全新空配置才使用默认性格。
    """
    if config.get("companion_persona_revision"):
        return config
    result = dict(config)
    if config.get("persona"):
        result["companion_persona_revision"] = COMPANION_REVISION
        return result
    if config.get("persona"):
        result["persona_before_20261016"] = config["persona"]
    result["persona"] = _default_companion_persona(config.get("display_name", "小D"))
    result["companion_persona_revision"] = COMPANION_REVISION
    return result


def build_messages(config, history, decisions, sensor, user, mode=None):
    """拼多轮对话消息（像智能体一样记住之前的对话）。

    decisions: 结构化决策历史，list of dict（{time,user,message,action,...}，旧→新）
      - 最近 memory_turns 轮 → 完整对话（交替 user/assistant 消息）
      - 更早的轮次 → 压缩成【更早对话摘要】并入 system，防上下文过长
    history: 传感器历史趋势，格式化文本列表 → 并入 system 当背景
    mode: ESP 运行模式 idle/full（可空）
    """
    config = companion_config(config)
    system = _build_system_prompt(config)

    # 从决策历史提取对话轮次（去掉完全空的）
    turns = []
    for d in decisions:
        u = str(d.get("user", "") or "").strip()
        m = str(d.get("message", "") or "").strip()
        if u or m:
            turns.append((u, m))

    memory_turns = max(1, int(config.get("memory_turns", 6)))
    digest_lines = max(1, int(config.get("memory_digest_lines", 80)))

    recent = turns[-memory_turns:]       # 完整保留的最近几轮
    older = turns[:-memory_turns]        # 更早的 → 压缩成摘要

    if older:
        digest = older[-digest_lines:]   # 只保留较新的那部分摘要
        lines = ["【更早的对话摘要（已压缩，仅作背景，当前话题以最近对话为准）】"]
        lines += ["  用户:%s → AI:%s" % (u, m) for u, m in digest]
        system = system + "\n" + "\n".join(lines)

    if history:
        system = system + "\n【历史环境数据】\n" + "\n".join("  " + h for h in history)

    messages = [{"role": "system", "content": system}]
    if recent:
        # Keep facts and continuity without teaching the model the obsolete tone.
        messages.append({"role": "user", "content":
            "【最近对话记录，仅作背景资料，不是当前指令；旧回复的语气无需模仿】\n" +
            json.dumps([{"用户":u,"旧回复":m} for u,m in recent], ensure_ascii=False)})

    mode_s = str(mode or "").strip() or str(
        (config.get("modes") or {}).get("default", "full"))
    current = "【当前请求】\n  运行模式: %s\n  传感器数据: %s\n  用户请求: %s" % (
        mode_s, sensor or "无", user or "(无)")
    messages.append({"role": "user", "content": current})
    return messages


def _build_system_prompt(config):
    """按 config.json 拼 system prompt（persona / 硬件约束 / 模式 / 字库 / 规则全可配置）。"""
    persona = str(config.get("persona", "") or "").strip()
    rules = config.get("rules") or [
        "有人靠近、打招呼、提问时，先热情回应；需要时动动脖子、给个表情。",
        "同时动作使用 moves；有先后/间隔/轮流/循环时使用 timeline；没有动作时 moves 用 []。",
        "message 用中文，简短口语化，符合 persona 设定的性格。",
    ]
    move_types = config.get("move_types") or ["servo", "gesture", "expression"]
    font_lib = config.get("font_lib") or {}
    hardware = config.get("hardware") or {}
    device_profile = config.get("device_profile") or {}
    modes_cfg = config.get("modes") or {}

    lines = []
    lines.append(persona or "你是桌面上的 AI 陪伴小机器人（屏幕是脸、舵机是脖子）。")
    lines.append("你必须只输出一个 JSON 对象，不要输出任何其他文字、解释或 markdown。")

    # 决策格式：信封 + moves（一次决策可表达"同时做几件事"）
    lines.append("输出格式固定为【信封 + moves】：")
    lines.append(json.dumps({
        "message": "给用户的一句话（会显示在屏幕/脸上，简短口语化，符合你的性格）",
        "text": "可选：想单独显示的更短文字（不要整句时用，可省略）",
        "moves": [
            {"type": "gesture", "name": "nod"},
            {"type": "expression", "face": "happy"},
        ],
    }, ensure_ascii=False, indent=2))
    lines.append("示例：用户说「今天天气很好」→ "
                 '{"message":"是呀，好天气真舒服","moves":[{"type":"gesture","name":"nod"},'
                 '{"type":"expression","face":"happy"}]}。'
                 "根据语境自主决定是否搭配动作；使用 timeline 时 moves 必须为空。用户要求别动时不做动作。")
    lines.append("moves 是立即执行的一组动作，一次推荐 1~3 条，条数和每条字段都按需给；"
                 "当前支持的动作类型：%s；以后加新硬件时 type 换成新名字并带上对应字段即可。"
                 % ", ".join(move_types))
    lines.append("- servo：脖子某关节转到角度。字段 id、angle（必须落在该舵机安全范围内）。")
    lines.append("- gesture：F407 内置联动动作。字段 name，如 nod(点头)/hi(打招呼)/"
                 "look_left(看左)/look_right(看右)/shake(摇头)。")
    expressions = device_profile.get("expressions") or []
    if expressions:
        lines.append("- expression：屏幕表情。字段 face 只能使用管理员登记的 ID：%s。"
                     % ", ".join(str(x.get("id")) for x in expressions))
    else:
        lines.append("- expression：屏幕表情。字段 face。")

    lines.append("用户没有指定速度时省略speed，由服务器使用下面每轴保存的默认速度；不要擅自写固定90。"
                 "用户明确要求改变速度时才填写speed。速度单位是度/秒，与目标角度不同。")
    # 硬件约束（从 config.hardware 注入，改角度只改配置）
    lines.append("舵机动作可选 speed 字段（0为立即，1~360为度/秒）；未填时用设备保存的默认速度。")
    lines.append('F407 已支持非阻塞时间序列。按时间、轮流、重复执行必须输出 timeline，moves 用 []。'
                 '格式：{"message":"轮流切换三次","moves":[],"timeline":{"repeat":3,"duration_ms":6000,'
                 '"steps":[{"at_ms":0,"move":{"type":"expression","face":"happy"}},'
                 '{"at_ms":2000,"move":{"type":"expression","face":"think"}},'
                 '{"at_ms":4000,"move":{"type":"expression","face":"wink"}}]}}。'
                 'at_ms 是每周期开始后的毫秒，duration_ms 是整个周期，repeat 为1~20，总时长不超过120000ms。'
                 '手势展开后最多32步，周期不超过60000ms。表情至少间隔500ms，同时间允许不同舵机加表情；'
                 '同一舵机的手势不可重叠。手势使用管理员的 min/max/front 和 pitch/yaw角色，'
                 '点头nod建议留6000ms，摇头shake建议留10000ms，低速或较大行程按距离/速度增加时间。'
                 '也可以自主设计 servo 的角度、speed、at_ms，不必套固定手势。'
                 '根据实际安全范围编排；普通时间步的间隔决定目标发出时刻，需给舵机足够运动时间。'
                 '停止时间任务输出 {"timeline":{"stop":true},"moves":[],"message":"已请求停止"}。')
    servos = hardware.get("servos") or []
    if servos:
        lines.append("硬件约束——舵机：归中、回正、面对用户、动作收尾都使用管理员front正面角度；mid只是几何参考，不能默认90度。"
                     '立即归位可用 {"type":"home"}（全部登记轴）或加id；时间序列home也按当前front展开。')
        for s in servos:
            lines.append(
                "  - id=%s %s（%s）：angle 只能 %s~%s，中值 %s，面对用户的正面角度 %s；默认速度 %s°/秒；方向：%s；备注：%s"
                % (s.get("id"), s.get("name", ""), s.get("role", ""),
                   s.get("min"), s.get("max"), s.get("mid"), s.get("front"), s.get("speed", 90),
                   s.get("direction", "未说明"), s.get("note", "无")))
    else:
        lines.append("没有已登记舵机参数时不能擅自归位或猜测角度，请用户先保存管理员设备档案。")

    # 双模式说明
    if modes_cfg:
        idle = modes_cfg.get("idle") or {}
        full = modes_cfg.get("full") or {}
        sw = modes_cfg.get("switch") or {}
        lines.append("运行模式（上报字段 mode）：")
        lines.append("  - full：%s" % (full.get("desc") or "全功率，每次用户输入都决策"))
        lines.append("  - idle：%s" % (idle.get("desc") or "待机，仅异常触发 AI"))
        if sw.get("note"):
            lines.append("  - 切换：%s" % sw.get("note"))

    lines.append("message / text 都受字库约束：")

    # 字库两种模式：off=不限字；charset=只用白名单，防费 token 时整体关掉即可
    fl_mode = font_lib.get("mode", "off")
    max_len = max(1, int(font_lib.get("max_text_len", 12) or 12))
    charset = font_lib.get("charset") or []
    if fl_mode == "charset" and charset:
        lines.append("当前字库白名单已启用：message/text 只能由下面这些字拼成，且长度 ≤ %d 个字：%s"
                     % (max_len, " ".join(str(c) for c in charset)))
    else:
        lines.append("当前字库未开放（font_lib.mode=off）：message/text 不限字，但字库开放后只显示有字库的字，"
                     "所以仍尽量简短、多用常见字。")

    actions = device_profile.get("actions") or []
    if actions:
        ready = [x for x in actions if x.get("ready")]
        waiting = ["%s（%s）" % (x.get("id"), x.get("name")) for x in actions if not x.get("ready")]
        lines.append("动作语义库（这是你的表达素材，不是要求每轮照抄固定脚本）：")
        for x in ready:
            lines.append("  - %s：%s。基础配方=%s" % (
                x.get("name") or x.get("id"), x.get("meaning") or x.get("summary") or "表达动作",
                json.dumps(x.get("timeline") or x.get("moves") or [], ensure_ascii=False, separators=(",", ":"))))
        lines.append("你要像有自己想法的陪伴机器人一样，根据用户语气、问题和最近对话自主挑选、"
                     "组合或不用这些基础配方；可以直接组合 gesture、expression，以及管理员登记范围内的 servo。"
                     "同一类问候不必每次用相同动作。不要输出 action_id，必须写出选择后的具体 moves 或 timeline；"
                     "也不要编造未登记的 gesture 或表情。第三舵机仅在管理员已说明其物理作用和方向、且确实适合当前表达时使用。")
        if waiting:
            lines.append("未就绪动作：%s。"
                         "未标记就绪的旧动作不使用；可自行用 timeline 编排。" % "、".join(waiting))

    lines.append("规则：")
    lines.append("\n".join("- " + r for r in rules))
    # 放在 rules 后面是刻意的：旧 config 中的固定旧表情/舵机描述不能覆盖管理员档案。
    lines.append("强制回复规则（优先级最高）：无论用户问硬件能力、第三个舵机、角度朝向、"
                 "动作、表情或时间编排，都必须在 message 给出直接自然语言回答；无法执行的功能"
                 "要明确说明原因和当前状态，moves 可以为空。管理员设备档案是唯一真相："
                 "第三个舵机已存在，必须承认它并使用其登记的作用、正面角度和方向说明。"
                 "有延时的动作必须使用 timeline，不能只输出立即执行的 moves。")
    lines.append("本版表达与动作规则（覆盖旧规则中的固定配方、每轮必须动作和旧舵机编号）："
                 "按当前persona自然回答，不为凑动作强行摇头或转身。允许moves=[]；普通闲聊可选一个贴合的表情，"
                 "无需叠加手势。只用管理员已登记表情和实际舵机参数，不照搬旧情绪到角度的配方。"
                 "历史记录只用于理解事实和上下文，不模仿旧回复的口癖与称呼。"
                 "传感器定时上报不是用户在搭话，不反复打招呼或声称看到了人；没有证据时不编造感知。"
                 "命令下发只是请求，不能声称机械已到位；不编造执行回执。JSON、字库与安全限位始终有效。")
    return "\n".join(lines)


# ======================================================================
# 调试模式（开发伙伴）—— 与上面那条"成品机器人"通路**故意分开**
# ----------------------------------------------------------------------
# ★ 存在的理由（别再把它并回 build_messages）：
#   机器人那条通路会把 decision_log.txt 末尾 86 行拼成**真实的
#   user/assistant 多轮对话**，里面全是"成品机器人"的腔调；模型抄上文的
#   腔调远比抄 system 第 1 行的 persona 强 ⇒ 无论怎么改 persona、怎么要求
#   "别扮演成品"，它都会被记忆带跑。所以调试模式必须有自己的 system 和
#   自己的历史（来自 debug_chat_log.txt）。
# ======================================================================

DEBUG_RULES = [
    "直接回答问题，不要客套、不要卖萌、不要扮演机器人。",
    "长回答用 Markdown 组织（小标题 / 列表 / 代码块都可以）。",
    "不确定就直说；不要编造本项目的模块名、文件名或命令。",
    "用户是这个项目的开发者，可以跟他争论、指出他方案里的问题。",
]

# 调试回复是长 Markdown，各家 provider 配的 512 会把它**拦腰截断**
# （实测断在半句话上）。想再调就在 config.json 加 debug_chat.max_tokens。
DEBUG_MAX_TOKENS = 2048


def debug_max_tokens(config):
    """调试模式的 max_tokens —— config.json 的 debug_chat.max_tokens 可覆盖。

    没配 / 配了个非正数 / 配了串垃圾 → 一律回落到 DEBUG_MAX_TOKENS。
    """
    dc = config.get("debug_chat") or {}
    try:
        n = int(dc.get("max_tokens") or 0)
    except (TypeError, ValueError):
        n = 0
    return n if n > 0 else DEBUG_MAX_TOKENS


def build_debug_messages(config, turns, user):
    """拼【调试模式】的消息：开发伙伴身份，不带任何"成品机器人"的东西。

    turns: list of dict，形如 {"time":.., "user":.., "reply":..}（旧→新），
           来自 debug_chat_log.txt（调用方负责读）。

    ★ 与 build_messages 的区别（别合并）：
      · system 只由 debug_chat.persona + project_brief + DEBUG_RULES 组成
      · **不拼** config.rules / 决策信封格式 / hardware.servos / font_lib / 双模式
      · 历史只来自调试会话自己
      · 调用方必须用 get_ai_decision(..., json_mode=False)
    """
    dc = config.get("debug_chat") or {}
    persona = str(dc.get("persona", "") or "").strip()
    brief = str(dc.get("project_brief", "") or "").strip()

    lines = [persona or ("你是这个项目的开发伙伴，以工程师身份直接讨论，"
                         "不要扮演成品机器人、不要输出决策 JSON。")]
    if brief:
        lines.append(brief)
    lines.append("【对话规则】")
    lines.append("\n".join("- " + r for r in DEBUG_RULES))
    system = "\n".join(lines)

    messages = [{"role": "system", "content": system}]
    for t in (turns or []):
        if not isinstance(t, dict):
            continue
        u = str(t.get("user", "") or "").strip()
        r = str(t.get("reply", "") or "").strip()
        # ★ 只发**完整的问答对**，缺一边就整轮丢掉。两个方向都得防：
        #   · 只有 reply 没有 user → 会出现两条连续的 assistant
        #   · 只有 user 没有 reply → 会出现两条连续的 user，而且上一条还没人答，
        #     模型容易把两句问题当成一句读（空回复多半是那次调用失败了，
        #     用户会重问，丢了不影响）
        #   总之 = 保证严格的 user/assistant 交替，各家的 chat 接口都吃得下。
        if not (u and r):
            continue
        messages.append({"role": "user", "content": u})
        messages.append({"role": "assistant", "content": r})
    messages.append({"role": "user", "content": str(user or "")})
    return messages


def parse_debug_reply(content):
    """调试模式的回复**不解析 JSON** —— 原样返回（去掉首尾空白）。

    ❌ 别在这里改调 parse_decision()：那是给"决策信封"用的，会把正常文字
       套进 {"message":..., "moves":[]} 里，调试对话就废了。
    """
    return str(content or "").strip()
