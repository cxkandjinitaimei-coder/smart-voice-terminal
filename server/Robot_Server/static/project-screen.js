/* Offline F407 framebuffer preview, not live board telemetry.
 * Layout: robot_gui.c::Gui_Frame. Face position: media.h::FACE_BOX_X/Y.
 * Keep the 240x320 physical texture; rotate the logical framebuffer with S3.
 * Static assets and their provenance live in robot-screen/source.json. */
export function createProjectScreen(T) {
  const expressions = ['blink','shy','happy','glasses','angry','funny','wink','pout',
    'laugh','starry','right','sleep','doubt','look','smile','surprise','think'];
  const revision = 'f407-rounded-20261018';
  const canvas = document.createElement('canvas');
  canvas.width = 240; canvas.height = 320;
  const ctx = canvas.getContext('2d');
  ctx.imageSmoothingEnabled = false;
  const texture = new T.CanvasTexture(canvas);
  texture.colorSpace = T.SRGBColorSpace;
  texture.magFilter = T.NearestFilter; texture.minFilter = T.LinearFilter;
  const material = new T.MeshBasicMaterial({map:texture, toneMapped:false});
  const images = {}, loaded = {}, errors = [];
  let orientation = 0, expression = 'blink';
  function draw() {
    const key = orientation === 90 ? 'landscape' : 'portrait';
    ctx.save();
    ctx.fillStyle = '#080f17'; ctx.fillRect(0,0,240,320);
    if (orientation === 90) { ctx.translate(240,0); ctx.rotate(Math.PI/2); }
    else if (orientation === 180) { ctx.translate(240,320); ctx.rotate(Math.PI); }
    if (loaded[key]) {
      ctx.drawImage(images[key],0,0);
      // The actual 160px SD frame fits inside the firmware's rounded face card.
      if (loaded[expression]) ctx.drawImage(images[expression],key==='landscape'?10:40,40,160,160);
    }
    ctx.restore();
    texture.needsUpdate = true;
  }
  draw();
  for (const key of ['portrait','landscape',...expressions]) {
    const img = new Image(); images[key] = img;
    img.onload = () => { loaded[key] = true; draw(); };
    img.onerror = () => { errors.push(key); };
    img.src = new URL(`./robot-screen/${key}.png?v=${revision}`,import.meta.url).href;
  }
  return {
    material, texture,
    setExpression(name) {
      if (!expressions.includes(name)) throw Error('未知表情');
      expression = name; draw();
    },
    setAngle(degrees) {
      const next = degrees>=135 ? 180 : degrees>=45 ? 90 : 0;
      if (next !== orientation) { orientation = next; draw(); }
    },
    info: () => ({orientation, expression, revision,
      ready:!!loaded.portrait && !!loaded.landscape,
      expressionsReady:expressions.every(name=>loaded[name]), errors:[...errors],
      resolution:[240,320], source:'F407 robot_gui.c rounded UI + FACE/*/0000.BMP',
      liveTelemetry:false})
  };
}
