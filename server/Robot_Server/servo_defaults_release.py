"""Finish the independent F407/web release without modifying historical packages."""
from datetime import datetime
import contextlib
import hashlib
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'AGENTS.md').is_file())
HERE = ROOT / 'flask_relay'
EVIDENCE = HERE / 'servo_defaults_validation'
OUT = ROOT / '交付/20261010/20261010_01_逐轴默认舵机速度与屏幕刷新'
APP_VERSION = 'V2026.10.10-01'
WEB_VERSION = 'W2026.10.10-01'
TASK = '[ArdProject] 逐轴默认舵机速度同步测试页/动作库/AI，慢速运动屏幕调度与刷新修复，F407和网页独立交付'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def historical_files():
    extensions = {'.py', '.ps1', '.bat', '.js', '.css', '.html', '.c', '.h', '.s',
                  '.uvprojx', '.bin', '.hex', '.axf', '.svg'}
    result = {}
    for directory in sorted((ROOT / '交付').glob('*/*')):
        if not directory.is_dir() or directory == OUT:
            continue
        for path in sorted(directory.rglob('*')):
            if (path.is_file() and not any(part in ('__pycache__', '.venv_lan',
                    'deploy_logs', 'lan_logs', 'logs', 'recordings') for part in path.parts)
                    and (path.suffix.lower() in extensions or path.name in ('SHA256.txt', 'release.json'))):
                result[path.relative_to(ROOT).as_posix()] = digest(path)
    return result


def baseline():
    assert not OUT.exists(), 'Delivery already exists; never replace a historical delivery'
    data = historical_files()
    (EVIDENCE / '历史发布文件_打包前.json').write_text(
        json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print('HISTORY_BASELINE files=' + str(len(data)))


def augment():
    from package_decoder_fix import FILES
    assert OUT.exists() and (OUT / 'release.json').exists()
    assert WEB_VERSION in (HERE / 'templates/index.html').read_text(encoding='utf-8')
    names = FILES + ['motion_timeline.py']
    for folder in ('网页一键更新', '网页局域网'):
        dest = OUT / folder
        dest.mkdir(exist_ok=True)
        for name in names:
            shutil.copy2(HERE / name, dest / name)
        for name in ('static', 'templates'):
            shutil.copytree(HERE / name, dest / name, dirs_exist_ok=True)
    lan = OUT / '网页局域网'
    for name in ('lan_start.py', 'start_lan.ps1', '一键启动局域网.bat',
                 'allow_lan_firewall.ps1', '允许局域网访问.bat'):
        shutil.copy2(HERE / name, lan / name)
    seeds = [
        '20261009_02_局域网网卡发现与上报状态修复',
        '20261008_06_互动动作按管理员参数编排',
        '20261008_05_上电按保存角度归位',
        '20261008_04_动作时间序列与可调舵机限位',
        '20261008_03_录音Flash后台写入与串口重传',
        '20261008_02_局域网语音识别与OTA',
    ]
    (lan / 'lan_seed.json').write_text(json.dumps({
        'source_config': '../../../' + seeds[0][:8] + '/' + seeds[0] + '/网页局域网/config.json',
        'fallback_configs': ['../../../' + name[:8] + '/' + name + '/网页局域网/config.json' for name in seeds[1:]],
    }, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    info = json.loads((OUT / 'release.json').read_text(encoding='utf-8'))
    assert info['version'] == APP_VERSION and info['app_bytes'] == 86600
    assert info['app_crc32'] == 'C687854D' and info['full_slot_a_crc32'] == 'E15C7098'
    info.update(web_version=WEB_VERSION, firmware_changed=True, update_components=['网页', 'F407'],
                hardware_verified=False, cloud_deployed=False, runtime_service_restarted=False,
                real_ai_api_called=False, esp_update_required=False, regression_suites_passed=14)
    (OUT / 'release.json').write_text(json.dumps(info, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    usage = (OUT / '验收说明.md').read_text(encoding='utf-8')
    (OUT / '使用说明.md').write_text(usage, encoding='utf-8')
    for folder in ('网页局域网', '网页一键更新'):
        (OUT / folder / 'README.md').write_text(usage, encoding='utf-8')
    firmware = OUT / 'firmware'
    objects = ROOT / 'ST32/AIAssistant/User/ObjectsOTA'
    for ext in ('hex', 'axf'):
        shutil.copy2(objects / ('MyAIMachines_OTA.' + ext), firmware / ('F407_APP_20261010_01.' + ext))
    for path in (ROOT / 'ST32/AIAssistant/User/ListingsOTA').glob('*.map'):
        shutil.copy2(path, firmware / path.name)
    # Include exact evidence and runnable tests; never include private runtime settings.
    validation = OUT / '验证'
    shutil.copytree(EVIDENCE, validation, ignore=shutil.ignore_patterns('__pycache__'))
    shutil.copy2(Path(__file__), validation / Path(__file__).name)
    tests = json.loads((EVIDENCE / 'regression.json').read_text(encoding='utf-8'))
    assert len(tests) == 14 and all(test['exit_code'] == 0 for test in tests)
    for test in tests:
        path = ROOT / test['test']
        target = validation / '测试源码' / test['test']
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
    (OUT / '本次变更.md').write_text('''# 本次变更

- 测试页新增逐轴“保存为默认”；保存只合并速度，不覆盖角度、限位、角色和表情。管理员页可编辑同一数据。
- 测试页刷新、AI上下文、互动动作库、回正及时间序列共同使用有效逐轴速度；未设置的轴继承目标设备通用速度，单次明确速度可覆盖，0保持立即动作。
- AI没有指定速度时由服务器补默认，不再猜固定90；语音文字进入AI的路径也使用实际目标设备参数。
- LCD绘图每行协作推进运动，慢速动作不再阻挡切页；AI页按表情/字幕变化局部更新，大图去掉先整区填黑。
- 保留录音、OTA、字库操作的互斥保护和现有0x20/0x22协议；ESP继续1.1.8。

完整文件和修改原因见HANDOFF与验收说明。软件验证通过，白屏是否全部消失仍需实板确认。
''', encoding='utf-8')
    print('AUGMENTED independent F407, LAN web, cloud web, verification')


def verify_package():
    from package_decoder_fix import FILES, seal, verify
    import lan_start
    assert OUT.exists()
    for folder in ('网页一键更新', '网页局域网'):
        for name in FILES + ['motion_timeline.py']:
            if name != 'README.md':
                assert (OUT / folder / name).read_bytes() == (HERE / name).read_bytes(), folder + '/' + name
        for name in ('static', 'templates'):
            for path in (HERE / name).rglob('*'):
                if path.is_file():
                    assert (OUT / folder / path.relative_to(HERE)).read_bytes() == path.read_bytes(), str(path)
        for secret in ('config.json', 'admin_device_profile.json', 'device_controls.json',
                       'audio_state.json', 'audio_retention.json', 'relay.db'):
            assert not (OUT / folder / secret).exists(), 'Runtime data mistakenly packaged: ' + secret
    fwroot = ROOT / 'ST32/AIAssistant'
    source_files = list((OUT / 'F407源码').rglob('*'))
    for path in source_files:
        if path.is_file():
            assert path.read_bytes() == (fwroot / path.relative_to(OUT / 'F407源码')).read_bytes(), str(path)
    project = OUT / 'F407源码/User/MyAIMachines_OTA.uvprojx'
    tree = ET.parse(project)
    for node in tree.findall('.//FilePath'):
        if node.text and not node.text.startswith('$$'):
            path = (project.parent / node.text.replace('\\', '/')).resolve()
            assert path.is_file(), 'Keil missing source: ' + node.text
            assert path.is_relative_to(OUT / 'F407源码'), 'Keil source escapes snapshot: ' + str(path)
    # Check each actual seed/fallback, in scratch only; do not write into delivered web.
    hint = json.loads((OUT / '网页局域网/lan_seed.json').read_text(encoding='utf-8'))
    checked = 0
    for relative in [hint['source_config'], *hint['fallback_configs']]:
        seed = (OUT / '网页局域网' / relative).resolve()
        if not seed.is_file():
            continue
        original = seed.read_bytes()
        with tempfile.TemporaryDirectory(prefix='servo-defaults-seed-') as temp:
            target = Path(temp)
            with contextlib.redirect_stdout(io.StringIO()):
                config = lan_start.setup_config(target, str(seed))
            source_config = json.loads(original.decode('utf-8-sig'))
            assert config == {**source_config, 'host': '0.0.0.0', 'port': 5000}
            for name in ('admin_device_profile.json', 'device_controls.json', 'audio_state.json', 'audio_retention.json'):
                if (seed.parent / name).is_file():
                    assert (target / name).read_bytes() == (seed.parent / name).read_bytes(), name
        assert seed.read_bytes() == original
        checked += 1
    assert checked
    baseline_data = json.loads((EVIDENCE / '历史发布文件_打包前.json').read_text(encoding='utf-8'))
    assert historical_files() == baseline_data, 'Historical published code/firmware changed while packaging'
    report = {'web_snapshots_match_live_code': True, 'f407_snapshot_matches_live_code': True,
              'keil_all_project_sources_present': True, 'no_runtime_secrets_packaged': True,
              'config_seeds_checked': checked, 'historical_published_files_unchanged': len(baseline_data),
              'version': APP_VERSION, 'web_version': WEB_VERSION, 'esp_version': '1.1.8'}
    (OUT / '验证/交付核验.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    seal(OUT)
    print('PACKAGE_VERIFIED SHA_entries=' + str(verify(OUT)) + ' history_files=' + str(len(baseline_data)))


def finish():
    stamp = datetime.now().strftime('%Y-%m-%d %H:%M')
    handoff = f'''最近更新: {stamp}
最后操作者: Codex

## 本次做了什么

- 用户测试页改左右180/上下270°每秒但AI仍90；确认PCA9685独立供电。修复测试速度未持久保存的断点：逐轴保存、管理员编辑、AI上下文/语音决策/动作库/home/timeline统一，显式单次速度可覆盖，0保留。目标角度仍遵守管理员限位/front。
- 慢速运动原先延后整页绘图和切页，现移除运动忙的切页阻挡、LCD每行协作推进舵机、AI页局部更新/大BMP不先整区填黑；保留流式录音/OTA/字库互斥。白屏是否彻底消失待实板验证，未假定全由供电造成。
- 已prepare --date 2026-10-10 --f407-only，真实Keil全量0E0W/fromelf BIN；14套回归通过（含真实Edge和实际C函数显示调度）。F407 {APP_VERSION} / 86600B / raw C687854D / full SlotA E15C7098；网页{WEB_VERSION}；ESP保持1.1.8无需刷新。
- 新独立包：{OUT}；完整Keil工程、BIN/HEX/AXF/MAP、LAN和云更新网页、验收/变更/SHA/验证。历史包代码和固件打包前后哈希一致，全部旧包保留。活工程已同步，未替用户刷板、部署、停服务或真实调用AI。

## 改了哪些文件

- flask_relay/device_profile.py（可选speed、锁内合并/原子保存、有效速度）；app.py（登录保护/api/admin/servo-speeds、实际device_id贯穿AI/队列、动作展开）；motion_timeline.py、ai_relay.py。
- flask_relay/static/page-test.js（保存默认、刷新参数、删除未定义SERVO_GROUPS）；page-interact.js（逐轴速度摘要）；page-settings.js；templates/index.html（网页版本及通用备用速度说明）。
- ST32/AIAssistant/Hardware/15LCD/lcd.c/.h（行间服务回调/重入保护/矩形检查）；App/media.c/.h（局部更新与BMP清屏）；App/ailink.c（协作服务互斥一致）；User/main.c（切页与状态绘图调度）、User/app_version.h。
- flask_relay/test_servo_defaults.py、test_servo_defaults_browser.py、servo_defaults_validation/（14套结果/Keil/显示C harness/上轮交接）、servo_defaults_release.py及新交付目录。
- 根HANDOFF与Hub任务/状态/日志收尾。本轮不git提交，原有大量dirty/untracked保留。

## 遗留问题与坑

- 用户先停旧5000服务再启动新包LAN并Ctrl+F5；首次继承09_02配置，缺失回退06→05→04→03→02→活项目。原录音/数据库留原目录，新记录在新包。云端脚本保留配置与历史，但未实际部署。
- 测试页每轴改值后必须“保存为默认”；单次测试输入不会自动影响AI。管理员角色决定上下/左右轴，勿固定假设1/2。旧@SWEEP连续扫动不携带轴speed，本轮验收定点/点头/摇头/home。每轴0~360度每秒，角度0~180仍按既有机械范围。
- 显示调度软件验证通过；白屏/慢转实际抖动仍需用户实板对照。PCA独立供电也可能有共地、瞬态或LCD总线问题，没加凭空LCD自动重初始化。
- OTA K-UP确认，SlotA08008000，BIN是OTA应用而不是Bootstrap；HEX也只应用。full SlotA CRC与raw文件CRC不能混用。中文字C/H保存UTF8 BOM，uvprojx无BOM；旧05用户保存副本的原SHA已有变更，不能重封掩盖。
- 既往网络/录音待验仍保留：多网卡发现09_02修复已有证据但ESP阻塞/供电/串口未排除；ESP录音/上传暂停取网页指令，重连和OTA查询有阻塞。公网尾延迟≤1秒与长期稳定性未保证，看门狗未接入。
- 报告图资料docs/项目/系统框图/及ZIP保留；用户六节正文以聊天63182编辑快照为准，阶段二三/心得84217/84218。不要覆盖用户手改报告。上轮完整交接在servo_defaults_validation/上轮HANDOFF.md。

## 给下一个 AI 的交接点

1. 用户启动新LAN或部署新云网页，先确认{WEB_VERSION}，保存上下270/左右180，刷新与互动摘要一致；动作库和AI点头/摇头/回正复验。真实AI未调用，若某次故意给speed需区分模型单次覆盖与默认同步。
2. F407用本包BIN OTA，屏/串口确认{APP_VERSION}/full CRC E15C7098；安全慢速运动时切页/同脸文字更新复验。仍全白先用断开舵机V+、保留PCA逻辑和共地的对照找电气或LCD问题，不假称已修复。
3. 回归入口python -X utf8 flask_relay/servo_defaults_validation/run_regression.py（14套已通过，不无故重跑）；独立包源/BIN/Keil与哈希核验在验证。apply_fix/apply_display_fix为已执行的一次性脚本，不重复运行。
4. 本次prepare已经执行，勿给本包再次prepare改版本。下次新固件按AGENTS先prepare，再实际Keil/fromelf，独立package；所有历史不覆盖。没有git reset/clean/全stage或提交。
'''
    (ROOT / 'HANDOFF.md').write_text(handoff, encoding='utf-8')
    (OUT / 'HANDOFF.md').write_text(handoff, encoding='utf-8')
    hub = Path('D:/AI-Agents-Hub')
    todo = hub / 'todo.md'
    text = todo.read_text(encoding='utf-8-sig')
    assert '- [ ] ' + TASK in text, 'Claimed Hub task missing'
    text = text.replace('- [ ] ' + TASK, '- [x] ' + TASK)
    text += f'\n- [ ] [ArdProject] {APP_VERSION}/{WEB_VERSION}实板：保存上下270/左右180，AI/动作库速度一致，慢转切页/白屏对照、OTA版本确认；软件交付已完成。 {stamp}\n'
    todo.write_text(text, encoding='utf-8')
    state_path = hub / 'state.json'
    state = json.loads(state_path.read_text(encoding='utf-8-sig'))
    project = state['projects']['ArdProject']
    project.setdefault('completed', []).append(stamp + ' 逐轴默认速度统一测试页/管理员/AI/动作库，LCD行间运动协作和局部刷新；F407 V2026.10.10-01、网页W2026.10.10-01独立包，Keil0E0W/14套回归；旧历史保留。')
    issue = 'V2026.10.10-01/W2026.10.10-01逐轴速度与慢速显示实板待验；软件切页/刷新修复通过，电气/LCD白屏原因未排除；未部署或刷板。'
    if isinstance(project.get('known_issues'), list):
        project['known_issues'].append(issue)
    else:
        project['known_issues'] = str(project.get('known_issues', '')) + '；' + issue
    project['services_status'] = '新01独立F407/两套网页交付完成，活Keil已同步；运行网页/云服务和设备未切换，ESP1.1.8保持。用户更新后实板验收。'
    data = project.setdefault('data', {})
    data['latest_servo_defaults_display_delivery'] = str(OUT)
    data['latest_lan_delivery'] = str(OUT / '网页局域网')
    data['latest_web_version'] = WEB_VERSION
    data['latest_f407_release'] = {'version': APP_VERSION, 'path': str(OUT / 'firmware/F407_OTA_20261010_01.bin'),
                                  'bytes': 86600, 'crc32': 'C687854D', 'full_slot_a_crc32': 'E15C7098'}
    state_path.write_text(json.dumps(state, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    with (hub / 'worklog.md').open('a', encoding='utf-8') as stream:
        stream.write(stamp + '  Codex | ' + TASK + ' 完成新01独立F407/局域网/云网页交付；Keil0E0W/14套回归，旧包保留，ESP无需刷；白屏实板待验，服务未切换。\n')
    verify_package()
    print('FINISHED ' + str(OUT))


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    {'baseline': baseline, 'augment': augment, 'verify': verify_package, 'finish': finish}[sys.argv[1]]()
