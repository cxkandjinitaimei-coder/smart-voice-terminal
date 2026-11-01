"""Independent web-only interaction action update; preserve firmware and history."""
from datetime import datetime
import json
from pathlib import Path
import re
import shutil
import sys

ROOT=next(p for p in Path(__file__).resolve().parents if (p / 'AGENTS.md').is_file())
HERE=ROOT/'flask_relay'
HUB=Path('D:/AI-Agents-Hub')
EVIDENCE=HERE/'action_validation'
TASK='[ArdProject] 补齐互动页已有动作：当前限位/速度/front时间编排、回正按钮及参数实时刷新'
VERSION='W2026.10.08-06'
OUT=ROOT/'交付/20261008/20261008_06_互动动作按管理员参数编排'


def claim():
    stamp=datetime.now().strftime('%Y-%m-%d %H:%M')
    EVIDENCE.mkdir(exist_ok=True)
    shutil.copy2(ROOT/'HANDOFF.md',EVIDENCE/'上轮HANDOFF.md')
    state=json.loads((HUB/'state.json').read_text(encoding='utf-8-sig'))
    state['projects']['ArdProject']['services_status']='互动页已有动作增量修复中；F407/ESP保持05，旧包及运行服务未改。'
    (HUB/'state.json').write_text(json.dumps(state,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    for name,content in [('todo.md','- [ ] '+TASK+'；claimed by Codex '+stamp),
                         ('worklog.md',stamp+'  Codex | '+TASK+' 开始；用户指出互动页动作入口遗漏。')]:
        with (HUB/name).open('a',encoding='utf-8') as f:f.write('\n'+content+'\n')


def package():
    from package_decoder_fix import FILES,seal,verify
    assert VERSION in (HERE/'templates/index.html').read_text(encoding='utf-8')
    previous=ROOT/'交付/20261008/20261008_05_上电按保存角度归位'
    print('05 preserved SHA entries='+str(verify(previous)))
    OUT.mkdir(parents=True, exist_ok=False)
    public=OUT/'网页一键更新';public.mkdir()
    for name in FILES+['motion_timeline.py']:shutil.copy2(HERE/name,public/name)
    lan=OUT/'网页局域网';lan.mkdir()
    for name in FILES+['motion_timeline.py','lan_start.py','start_lan.ps1','一键启动局域网.bat',
                      'allow_lan_firewall.ps1','允许局域网访问.bat']:
        if name not in ['deploy_web.ps1','一键更新网页到云端.bat','一键更新网页到云端.ps1','README.md']:
            shutil.copy2(HERE/name,lan/name)
    for dest in [public,lan]:
        for folder in ['static','templates']:shutil.copytree(HERE/folder,dest/folder)
    (lan/'lan_seed.json').write_text(json.dumps({
        'source_config':'../../../20261008/20261008_05_上电按保存角度归位/网页局域网/config.json',
        'fallback_configs':['../../../20261008/20261008_04_动作时间序列与可调舵机限位/网页局域网/config.json',
                            '../../../20261008/20261008_03_录音Flash后台写入与串口重传/网页局域网/config.json',
                            '../../../20261008/20261008_02_局域网语音识别与OTA/网页局域网/config.json']},ensure_ascii=False)+'\n',encoding='utf-8')
    validation=OUT/'验证';validation.mkdir()
    for p in EVIDENCE.iterdir():
        if p.is_file():shutil.copy2(p,validation/p.name)
    for name in ['test_action_library.py','test_action_browser.py','run_action_regression.py','action_release.py']:
        shutil.copy2(HERE/name,validation/name)
    old=json.loads((previous/'release.json').read_text(encoding='utf-8'))
    meta={'version':VERSION,'web_version':VERSION,'firmware_changed':False,
          'f407_version':old['version'],'esp_version':old['esp_version'],
          'update_boards':['WEB'],'firmware_source_delivery':previous.name,
          'cloud_deployed':False,'hardware_verified':False,'regression_suites':8,
          'actions_use_current_profile':True,'zero_speed_preserved':True}
    (OUT/'release.json').write_text(json.dumps(meta,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    usage=f'''# {VERSION} 互动页已有动作修复

本轮只更新网页；F407继续使用V2026.10.08-05、ESP继续1.1.8。若还没安装05，先按旧05包验收说明更新两板；已是05则不用重新烧录。屏幕固件版本保持05，网页显示06。

## 使用

- 局域网：关闭旧网页启动窗口，双击本包“网页局域网/一键启动局域网.bat”，保持窗口打开，访问启动窗口给出的局域网网址。首次优先继承05参数，若05尚无运行配置则继承04，再03/02。原档案不修改，ASR模型复用。
- 公网：双击本包“网页一键更新/一键更新网页到云端.bat”，等DEPLOY_OK后刷新。云端config、管理员参数、录音及数据库保留。助手没有代部署或更改运行中的服务。
- 刷新后确认网页版本{VERSION}，切到“互动”。原点头、摇头、打招呼、看左/右、连续点头按钮都还在，并新增“回正”。

## 验收

1. 动作库上方显示当前保存的每轴限位、正面角度和手势速度。你的已保存参数为30/60/90；新包继承运行中的最新档案，不把角度写回默认90。
2. 点头：pitch最小→最大→最小→front；摇头：yaw最小→最大→front；打招呼：pitch最大→最小→最大→front。完整动作一次入队，F407按时间执行，所有步骤使用当前保存速度。看左停在yaw最小，看右停在yaw最大；“回正”使全部登记舵机回到各自front。
3. 管理员改front或限位、保存，回互动页后参数/按钮提示应刷新；停留互动页时也会随正常轮询刷新。下一次点击按服务端当前保存值编排，即使按钮刚才显示的是旧参数，也不会发送旧角度。
4. 速度0仍表示每步立即设目标，步骤之间保留200ms停留；非0按度/秒计算行程时间。连续点头两次也使用当前速度，不再固定90度/秒。
5. 缺pitch/yaw或对应表情时，按钮禁用并提示原因；不再笼统说“硬件还没实现”。开心/思考/惊讶/眨眼仍按登记BMP文件名发送。

新动作会替换F407尚未完成的时间任务；录音、OTA和字库暂停仍由05固件处理。页面“已入队”只证明进了队列，不是实际轴位置反馈。AI动作库上下文也同步新的配方和摘要，仍允许自由组合时间、角度、表情和动作。

原05目录保留，本包不包含新固件，完整Keil源和两板固件继续在05目录。3D姿态页的本地几何校准和“全部归中”属于另一个入口，本次未改。

8套软件回归通过：真实接口/旧配方迁移/改参数后再点击、真Edge互动按钮与管理员编辑、原归位和时间表浏览器、AI时间表/档案、对话投递、LAN启动、部署。脚本与日志在“验证”，复跑入口是活项目flask_relay/run_action_regression.py。实板动作仍待用户验收。
'''
    (OUT/'使用说明.md').write_text(usage,encoding='utf-8')
    for dest in [public,lan]:(dest/'README.md').write_text(usage,encoding='utf-8')
    # Verify exact web copies and real first-start settings inheritance without
    # starting servers, contacting boards, or copying settings into the release.
    import contextlib,io,tempfile
    import lan_start
    hint=json.loads((lan/'lan_seed.json').read_text(encoding='utf-8'))
    choices=[hint['source_config']]+hint['fallback_configs']
    seed=next((lan/name).resolve() for name in choices if (lan/name).is_file())
    profile_before=(seed.parent/'admin_device_profile.json').read_bytes()
    with tempfile.TemporaryDirectory(prefix='action-package-') as temp:
        with contextlib.redirect_stdout(io.StringIO()):lan_start.setup_config(Path(temp),str(seed))
        assert (Path(temp)/'admin_device_profile.json').read_bytes()==profile_before
    assert (seed.parent/'admin_device_profile.json').read_bytes()==profile_before
    for dest in [public,lan]:
        for name in FILES+['motion_timeline.py']:
            if name!='README.md' and (dest/name).exists():assert (dest/name).read_bytes()==(HERE/name).read_bytes()
        for folder in ['static','templates']:
            for p in (HERE/folder).rglob('*'):
                if p.is_file():assert p.read_bytes()==(dest/p.relative_to(HERE)).read_bytes()
    (validation/'交付核验.log').write_text('PASS both web snapshots match live code; real LAN first-start copied previous saved profile unchanged; original settings/history and firmware05 preserved.\n',encoding='utf-8')
    stamp=datetime.now().strftime('%Y-%m-%d %H:%M')
    handoff=f'''最近更新: {stamp}
最后操作者: Codex

## 本次做了什么

- 用户指出互动页已有动作未同步。点头/摇头/打招呼/看左/看右/连续点头改为按管理员当前角色ID、min/max/front及默认速度生成完整timeline；新增home回正。旧保存动作ID自动用新程序配方，档案角度与旧文件未改。AI动作上下文同用新配方，保持自由组合。
- 互动动作库展示实时参数与目标摘要，每轮刷新但未变化不重建按钮；点击时服务端重新读取同一档案快照编排，缺舵机/表情或无效时间表禁用/拒绝且不入队。删除旧“尚未实现时间队列”说法。入队反馈不冒充实际舵机完成。
- 修复时间动作默认速度0被or90替换；0按每步立即目标+200ms停留，非0按距离/度每秒计算时间，连续点头不固定90。无需改两板固件。
- 8套软件回归通过（真实接口/旧档案/角色重映射/参数变化/0速度，真Edge8个动作按钮、管理员保存和互动实时刷新/禁用，以及原时间表/归位UI、AI、档案、对话投递、LAN启动、部署）。新独立交付/{OUT.name}，网页{VERSION}；F407继续05(86444B raw85A66763 全槽C99A7BBC)、ESP1.1.8，屏固件仍05。05及旧交付哈希保留，未部署公网或烧板。

## 改了哪些文件

- flask_relay/device_profile.py、app.py、motion_timeline.py、static/page-interact.js、templates/index.html。
- flask_relay/test_device_profile.py、test_action_library.py、test_action_browser.py、run_action_regression.py、action_release.py、action_validation/。
- 新06两个网页入口、参数继承配置、使用说明、release.json、验证证据/SHA；根与ST32 HANDOFF、Hub任务/日志/状态。

## 遗留问题与坑

- 本轮只有网页，不重编/改ESP和F407。所有互动功能配套05固件；若用户还未安装05，需先按05验收说明升级。屏05/网页06是正常的不同组件版本，网页升级不改变屏幕固件版本。
- 新LAN先继承05运行配置，缺时04→03→02→root；旧录音仍原目录，新队列独立。公网尚未部署本轮，使用06网页一键更新入口，依赖/ASR小模型继承既有修复不重新下载。
- 3D姿态页是独立本地几何模型：page-pose.js::pose-center仍设置模型90；本次只修用户点名的互动动作库。不要宣称所有网页入口都无90默认。
- 05上电30/60/90/离线缓存待实板：首次联网确认MCU限位同步及ESP缓存已保存，断网重启使用最后保存值，擦ESP FS会丢缓存。PCA无实际轴反馈；OTA仍K-UP/Slot A0x08008000，不能SWD仅覆盖A留下旧CRC。Bootloader/字库/引脚未改。
- 03音频Flash/串口重传修复实板仍待复验，公网<=1秒尾延迟未保证；看门狗未接入。git旧dirty/untracked未reset/clean/全stage/提交。本輪交接前状态在action_validation/上轮HANDOFF.md，05完整Keil和固件继续保留。

## 给下一个 AI 的交接点

1. 用户启动06网页局域网或部署06公网网页，看到W06后在互动页试点头/摇头/打招呼/连续点头与回正；先确保两板是05。改front后再点，UI摘要和最后回正必须跟着改。
2. python flask_relay/run_action_regression.py 跑8套。本轮不需要再跑Keil或Arduino；以前固件16套见home_validation。新动作完整一次入队，raw无#也会由05ESP识别为0x20二进制，home走@MOVE:home,all。
3. 后续若用户要修3D姿态页，先处理page-pose.js和pose-parts.js的本地行程/模型零位与管理员front关系，不能仅发home却让模型显示夹取后的不同角度，也不能顺手覆盖用户几何校准。
4. 新固件依旧release_audio.py prepare按用户日期再真编译和package独立历史。06是web-only历史目录，下一次固件序号至少07。旧05缓存/OTA机制不重做。
'''
    for p in [ROOT/'HANDOFF.md',ROOT/'ST32/AIAssistant/HANDOFF.md',OUT/'HANDOFF.md']:p.write_text(handoff,encoding='utf-8')
    todo=HUB/'todo.md';text=todo.read_text(encoding='utf-8-sig').replace('- [ ] '+TASK,'- [x] '+TASK)
    text+='\n- [ ] [ArdProject] W2026.10.08-06实板：更新网页，05固件上验互动动作新限位/front/速度、连续点头、回正。 '+stamp+'\n'
    todo.write_text(text,encoding='utf-8')
    state=json.loads((HUB/'state.json').read_text(encoding='utf-8-sig'));project=state['projects']['ArdProject']
    project.setdefault('completed',[]).append('2026-10-08 W06互动页旧动作入口补齐：当前参数timeline、回正、实时刷新/缺参数禁用/速度0保护，8套回归和独立网页历史包；05两板固件保留。')
    issue='W06互动动作实板待验收，网页需更新、两板继续05；3D姿态模型归中入口仍为本地90，与互动动作库独立。'
    if isinstance(project.get('known_issues'),list):project['known_issues'].append(issue)
    else:project['known_issues']=str(project.get('known_issues',''))+'；'+issue
    project['services_status']='W06独立网页包完成，05固件不变；旧服务/公网未部署，互动动作实板待用户验收。'
    (HUB/'state.json').write_text(json.dumps(state,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    with (HUB/'worklog.md').open('a',encoding='utf-8') as f:f.write(stamp+'  Codex | '+TASK+' 完成W06网页独立包；8套回归通过，05固件/旧配置/历史保留，未部署或烧板。\n')
    seal(OUT);print(VERSION+' SHA entries='+str(verify(OUT)))
    print('05 preserved SHA entries='+str(verify(previous)))


def update_evidence():
    from package_decoder_fix import seal,verify
    assert OUT.is_dir()
    for name in ['test_action_browser.py','action_release.py']:
        shutil.copy2(HERE/name,OUT/'验证'/name)
    for p in EVIDENCE.iterdir():
        if p.is_file() and p.suffix in ['.log','.png']:shutil.copy2(p,OUT/'验证'/p.name)
    seal(OUT);print(VERSION+' verified SHA entries='+str(verify(OUT)))


if __name__=='__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    {'claim':claim,'package':package,'evidence':update_evidence}[sys.argv[1]]()
