// Makes one PDF of the guides people read: the user's guide, the tutorials,
// the command-line reference and the plugin guide, from the same docs/ the
// site is made from, so it's as current as the site. Written to
// dist/site/sketchbillet-guides.pdf, linked from every guide page. Runs after
// prerender.mjs (see package.json), with Chromium through Playwright.

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { chromium } from "playwright";
import { loadDocs } from "./docs.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "..");
const repo = path.resolve(root, "..");
const out = path.join(root, "dist", "site");
const SITE_URL = "https://sketchbillet.org";
export const PDF_NAME = "sketchbillet-guides.pdf";

// The sections in the PDF; the design notes are for SketchBillet's own developers.
const IN_PDF = ["guide", "tutorials", "cli", "plugins"];

const esc = (s) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/"/g, "&quot;");
const slug = (route) => route.replace(/^\/|\/$/g, "").replace(/\//g, "-") || "home";

const docs = loadDocs(repo, out);
const pages = docs.pages.filter((p) => IN_PDF.includes(p.section));
const inPdf = new Map(pages.map((p) => [p.route, slug(p.route)]));

// One page's HTML, made to fit in the PDF: its ids prefixed with the page's own
// name (pages share heading names, like "Tips"), links to other pages in the PDF
// turned into links within it, links to the rest of the site made whole, and
// pictures found in the built site.
function forPdf(page) {
  const own = inPdf.get(page.route);
  return page.html
    .replace(/\sid="([^"]+)"/g, (_, id) => ` id="${own}--${id}"`)
    .replace(/\shref="([^"]+)"/g, (whole, href) => {
      if (href.startsWith("#")) return ` href="#${own}--${href.slice(1)}"`;
      if (!href.startsWith("/")) return whole; // another website, or mailto:
      const [route, frag] = href.split("#");
      const target = inPdf.get(route.endsWith("/") ? route : route + "/");
      if (target) return ` href="#${frag ? `${target}--${frag}` : target}"`;
      return ` href="${SITE_URL}${href}"`;
    })
    .replace(/\ssrc="\/([^"]+)"/g, (_, p) => ` src="${pathToFileURL(path.join(out, p)).href}"`);
}

const today = new Date().toISOString().slice(0, 10);
let commit = "";
try {
  commit = fs.readFileSync(path.join(repo, ".git", "HEAD"), "utf8").trim();
  if (commit.startsWith("ref: ")) commit = fs.readFileSync(path.join(repo, ".git", commit.slice(5)), "utf8").trim();
} catch {
  commit = process.env.GITHUB_SHA || "";
}

const sections = docs.sections.filter((s) => IN_PDF.includes(s.id));
const contents = sections
  .map(
    (s) => `<li><a href="#${inPdf.get(s.base)}">${esc(s.title)}</a><ol>${s.pages
      .filter((p) => p.route !== s.base)
      .map((p) => `<li><a href="#${inPdf.get(p.route)}">${esc(p.title)}</a></li>`)
      .join("")}</ol></li>`,
  )
  .join("");
const body = pages
  .map((p) => {
    const sectionStart = sections.some((s) => s.base === p.route);
    const section = sections.find((s) => s.id === p.section);
    return `<section class="page${sectionStart ? " section-start" : ""}" id="${inPdf.get(p.route)}">
  <p class="section-name">${esc(section.title)}</p>
  <h1>${esc(p.title)}</h1>
  ${forPdf(p)}
</section>`;
  })
  .join("\n");

const fontCss = ["barlow/400.css", "barlow/600.css", "barlow-condensed/600.css", "ibm-plex-mono/400.css"]
  .map((f) => `<link rel="stylesheet" href="${pathToFileURL(path.join(root, "node_modules", "@fontsource", f)).href}">`)
  .join("\n");

const html = `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>SketchBillet guides</title>
${fontCss}
<style>
  @page { size: Letter; margin: 18mm 16mm 20mm; }
  body { font-family: "Barlow", system-ui, sans-serif; font-size: 10.5pt; line-height: 1.45; color: #1f2328; }
  h1, h2, h3, h4 { font-family: "Barlow Condensed", "Barlow", sans-serif; font-weight: 600; line-height: 1.15;
    break-after: avoid; }
  h1 { font-size: 22pt; margin: 0 0 10pt; }
  h2 { font-size: 15pt; margin: 18pt 0 6pt; }
  h3 { font-size: 12.5pt; margin: 14pt 0 4pt; }
  a { color: #1f5fbf; text-decoration: none; }
  p, li { orphans: 3; widows: 3; }
  code, pre { font-family: "IBM Plex Mono", ui-monospace, monospace; font-size: 8.5pt; }
  code { background: #f2f4f7; padding: 0 2px; border-radius: 2px; }
  pre { background: #f6f8fa; border: 1px solid #e3e7ec; border-radius: 4px; padding: 6pt 8pt;
    white-space: pre-wrap; overflow-wrap: anywhere; }
  pre code { background: none; padding: 0; }
  table { border-collapse: collapse; width: 100%; margin: 6pt 0 10pt; font-size: 9.5pt; break-inside: auto; }
  th, td { border: 1px solid #d8dde3; padding: 3pt 5pt; text-align: left; vertical-align: top; }
  th { background: #f2f4f7; }
  tr { break-inside: avoid; }
  img { max-width: 100%; max-height: 120mm; display: block; margin: 6pt auto; break-inside: avoid; }
  blockquote { margin: 6pt 0; padding: 2pt 10pt; border-left: 3px solid #d8dde3; color: #4b535c; }
  .cover { height: 230mm; display: flex; flex-direction: column; justify-content: center; break-after: page; }
  .cover h1 { font-size: 40pt; margin-bottom: 4pt; }
  .cover p { font-size: 13pt; margin: 2pt 0; color: #4b535c; }
  .contents { break-after: page; }
  .contents > ol { padding-left: 0; list-style: none; }
  .contents > ol > li { margin-top: 10pt; font-weight: 600; break-inside: avoid-page; }
  .contents ol ol { font-weight: 400; columns: 2; column-gap: 12mm; margin: 4pt 0 0; padding-left: 16pt; }
  .page { break-before: page; }
  .section-name { font-family: "Barlow Condensed", sans-serif; color: #6b7480; margin: 0; font-size: 10pt; }
</style>
</head>
<body>
<div class="cover">
  <h1>SketchBillet guides</h1>
  <p>The user's guide, tutorials, command-line reference and plugin guide, in one file.</p>
  <p>Made from the guides on ${esc(today)}${commit ? `, at version ${esc(commit.slice(0, 10))}` : ""}. The newest are always at ${SITE_URL}.</p>
</div>
<nav class="contents"><h1>Contents</h1><ol>${contents}</ol></nav>
${body}
</body>
</html>`;

const htmlFile = path.join(root, "dist", "guides-pdf.html");
fs.writeFileSync(htmlFile, html);
const browser = await chromium.launch();
try {
  const page = await browser.newPage();
  await page.goto(pathToFileURL(htmlFile).href, { waitUntil: "load" });
  await page.evaluate(() => document.fonts.ready);
  await page.pdf({
    path: path.join(out, PDF_NAME),
    format: "Letter",
    printBackground: true,
    displayHeaderFooter: true,
    headerTemplate: "<span></span>",
    footerTemplate:
      '<div style="width:100%;font:8pt sans-serif;color:#6b7480;text-align:center">SketchBillet guides · <span class="pageNumber"></span> of <span class="totalPages"></span></div>',
    margin: { top: "18mm", bottom: "20mm", left: "16mm", right: "16mm" },
    outline: true, // bookmarks: every page and heading
    tagged: true,
  });
} finally {
  await browser.close();
}
const size = fs.statSync(path.join(out, PDF_NAME)).size;
console.log(`wrote ${PDF_NAME}: ${pages.length} pages of the guides, ${(size / 1024 / 1024).toFixed(1)} MB`);
