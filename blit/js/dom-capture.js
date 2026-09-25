// Rasterize a DOM element, in place and with no permission prompt, into a
// <canvas>, which rasterize() / BleCast.sendImage() then take.
//
// Browsers have no DOM-to-bitmap API, so this does what html-to-image and
// friends do: clone the element with every computed style inlined, embed its
// web fonts and images as data: URLs, wrap it in an SVG <foreignObject>, and
// draw that SVG into a canvas. The result matches the page closely, with
// limits:
//
// - Images, fonts and stylesheets must be same-origin or served with CORS;
//   anything else can't be embedded and renders blank / in a fallback font.
// - <iframe> contents are never captured (the box stays empty).
// - Canvas, <video> and form fields are captured as they are right now.
//
//   const canvas = await captureElement(document.querySelector('#screen'));

const fetchCache = new Map(); // url -> Promise<data: URL | null>

/**
 * Capture `element` into a new canvas at its CSS size × `scale`.
 * Options: scale (default 1), background (default '#fff'; null = transparent).
 */
export async function captureElement(element, { scale = 1, background = '#fff' } = {}) {
  const rect = element.getBoundingClientRect();
  const width = Math.ceil(rect.width);
  const height = Math.ceil(rect.height);

  const pseudoRules = [];
  const clone = await cloneNode(element, pseudoRules);
  // The root sits at the SVG's origin, whatever its place in the page.
  Object.assign(clone.style, { margin: '0', position: 'relative', left: '0', top: '0', transform: 'none' });

  const fontCss = await embedFonts(usedFontFamilies(element));
  const style = document.createElement('style');
  style.textContent = fontCss + pseudoRules.join('\n');
  const wrapper = document.createElement('div');
  wrapper.append(style, clone);

  const xhtml = new XMLSerializer().serializeToString(wrapper);
  const svg =
    `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}">` +
    `<foreignObject x="0" y="0" width="100%" height="100%">${xhtml}</foreignObject></svg>`;
  const img = new Image();
  img.src = `data:image/svg+xml;charset=utf-8,${encodeURIComponent(svg)}`;
  await img.decode();

  const canvas = document.createElement('canvas');
  canvas.width = Math.round(width * scale);
  canvas.height = Math.round(height * scale);
  const ctx = canvas.getContext('2d');
  if (background) {
    ctx.fillStyle = background;
    ctx.fillRect(0, 0, canvas.width, canvas.height);
  }
  ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
  return canvas;
}

// --- cloning -----------------------------------------------------------------

let pseudoId = 0;

async function cloneNode(node, pseudoRules) {
  if (node.nodeType === Node.TEXT_NODE) return node.cloneNode(false);
  if (node.nodeType !== Node.ELEMENT_NODE) return null;
  const tag = node.tagName.toLowerCase();

  if (tag === 'script' || tag === 'style' || tag === 'link' || tag === 'noscript') return null;
  if (tag === 'canvas') return imageLike(node, safeDataUrl(() => node.toDataURL()));
  if (tag === 'video') return imageLike(node, videoFrame(node));
  if (tag === 'img') {
    const img = copyWithStyle(node, node.cloneNode(false));
    img.removeAttribute('srcset');
    img.removeAttribute('loading');
    const url = node.currentSrc || node.src;
    if (url) img.setAttribute('src', (await toDataUrl(url)) ?? url);
    return img;
  }
  if (node instanceof SVGElement && tag === 'svg') {
    // Inline SVG: copy it whole; its presentation comes along with it.
    return copyWithStyle(node, node.cloneNode(true));
  }

  const copy = copyWithStyle(node, node.cloneNode(false));
  if (tag === 'input') copy.setAttribute('value', node.value);
  if (tag === 'textarea') copy.textContent = node.value;
  if (tag === 'select') {
    for (const opt of node.options) if (opt.selected) opt.setAttribute('selected', '');
  }
  addPseudo(node, copy, '::before', pseudoRules);
  addPseudo(node, copy, '::after', pseudoRules);

  for (const child of node.childNodes) {
    const c = await cloneNode(child, pseudoRules);
    if (c) copy.append(c);
  }
  return copy;
}

function copyWithStyle(source, target) {
  target.removeAttribute('id'); // keep the page's CSS from matching the clone
  target.removeAttribute('class');
  const cs = getComputedStyle(source);
  let text = '';
  for (const prop of cs) text += `${prop}:${cs.getPropertyValue(prop)};`;
  target.setAttribute('style', text);
  return target;
}

function addPseudo(source, target, which, rules) {
  const cs = getComputedStyle(source, which);
  const content = cs.getPropertyValue('content');
  if (!content || content === 'none' || content === 'normal') return;
  const cls = `bc-p${++pseudoId}`;
  target.classList.add(cls);
  let text = '';
  for (const prop of cs) text += `${prop}:${cs.getPropertyValue(prop)};`;
  rules.push(`.${cls}${which}{${text}}`);
}

async function imageLike(node, dataUrlPromise) {
  const img = document.createElement('img');
  const url = await dataUrlPromise;
  if (url) img.setAttribute('src', url);
  copyWithStyle(node, img);
  return img;
}

function videoFrame(video) {
  if (!video.videoWidth) return Promise.resolve(null);
  const c = document.createElement('canvas');
  c.width = video.videoWidth;
  c.height = video.videoHeight;
  c.getContext('2d').drawImage(video, 0, 0);
  return safeDataUrl(() => c.toDataURL());
}

async function safeDataUrl(fn) {
  try {
    return fn(); // throws for a tainted (cross-origin) canvas
  } catch {
    return null;
  }
}

// --- resources -----------------------------------------------------------------

function toDataUrl(url) {
  if (url.startsWith('data:')) return Promise.resolve(url);
  if (!fetchCache.has(url)) {
    fetchCache.set(
      url,
      fetch(url, { mode: 'cors', credentials: 'omit' })
        .then((r) => (r.ok ? r.blob() : null))
        .then((blob) => blob && new Promise((resolve) => {
          const reader = new FileReader();
          reader.onload = () => resolve(reader.result);
          reader.onerror = () => resolve(null);
          reader.readAsDataURL(blob);
        }))
        .catch(() => null),
    );
  }
  return fetchCache.get(url);
}

function usedFontFamilies(root) {
  const families = new Set();
  const add = (el, pseudo) => {
    for (const f of getComputedStyle(el, pseudo).fontFamily.split(',')) families.add(f.trim().replace(/^["']|["']$/g, '').toLowerCase());
  };
  add(root);
  for (const el of root.querySelectorAll('*')) {
    add(el);
    add(el, '::before');
    add(el, '::after');
  }
  return families;
}

let fontFacesPromise = null;

// Every @font-face rule the page can see: same-origin sheets via CSSOM,
// cross-origin ones (e.g. Google Fonts) by fetching their text with CORS.
function pageFontFaces() {
  fontFacesPromise ??= (async () => {
    const faces = [];
    for (const sheet of document.styleSheets) {
      let rules = null;
      try {
        rules = sheet.cssRules;
      } catch {
        // Cross-origin sheet: read its source instead.
      }
      if (rules) {
        for (const rule of rules) if (rule instanceof CSSFontFaceRule) faces.push({ css: rule.cssText, base: sheet.href ?? location.href });
      } else if (sheet.href) {
        try {
          const text = await (await fetch(sheet.href, { mode: 'cors' })).text();
          for (const css of text.match(/@font-face\s*{[^}]*}/g) ?? []) faces.push({ css, base: sheet.href });
        } catch {
          // No CORS: that sheet's fonts fall back.
        }
      }
    }
    return faces;
  })();
  return fontFacesPromise;
}

// @font-face CSS for the used families, with font files inlined. Only the
// basic Latin subset of unicode-range-split fonts, to keep the SVG small.
async function embedFonts(families) {
  const out = [];
  for (const { css, base } of await pageFontFaces()) {
    const family = css.match(/font-family:\s*["']?([^;"']+)/)?.[1]?.trim().toLowerCase();
    if (!family || !families.has(family)) continue;
    const range = css.match(/unicode-range:\s*([^;}]+)/)?.[1];
    if (range && !/U\+0000|U\+0020|U\+0-/i.test(range)) continue;
    let inlined = css;
    for (const [, quote, url] of css.matchAll(/url\((["']?)([^"')]+)\1\)/g)) {
      const data = await toDataUrl(new URL(url, base).href);
      if (data) inlined = inlined.replace(`url(${quote}${url}${quote})`, `url("${data}")`);
    }
    out.push(inlined);
  }
  return out.join('\n');
}
