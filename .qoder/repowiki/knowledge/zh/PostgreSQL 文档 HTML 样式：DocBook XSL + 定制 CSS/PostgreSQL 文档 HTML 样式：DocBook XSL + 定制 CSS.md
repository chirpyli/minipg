---
kind: frontend_style
name: PostgreSQL 文档 HTML 样式：DocBook XSL + 定制 CSS
category: frontend_style
scope:
    - '**'
source_files:
    - doc/src/sgml/stylesheet.xsl
    - doc/src/sgml/stylesheet-common.xsl
    - doc/src/sgml/stylesheet-html-common.xsl
    - doc/src/sgml/stylesheet-speedup-xhtml.xsl
    - doc/src/sgml/stylesheet-fo.xsl
    - doc/src/sgml/stylesheet-hh.xsl
    - doc/src/sgml/stylesheet.css
    - doc/src/sgml/docguide.sgml
---

## 1. 使用的系统/方法

本仓库是 PostgreSQL 数据库源码，本身不包含 Web 前端应用（无 React/Vue/Angular、无 Tailwind、无 SCSS）。唯一的“前端样式”集中在**文档构建系统**中，采用 **DocBook SGML → XSLT → HTML/PDF** 的管线：
- 源文档位于 `doc/src/sgml/*.sgml`。
- 通过 DocBook XSLT 1.0 样式表将 SGML 转换为 XHTML chunked HTML、FO（PDF）、HTML Help、man page 等。
- 在 `stylesheet.xsl` 中 `import` 了上游 DocBook 样式 `http://docbook.sourceforge.net/release/xsl/current/xhtml/chunk.xsl`，并通过本地 `stylesheet-common.xsl`、`stylesheet-html-common.xsl`、`stylesheet-speedup-xhtml.xsl` 进行覆盖与增强。
- 最终 HTML 页面的视觉外观由 `doc/src/sgml/stylesheet.css` 提供，该文件被 XSLT 参数 `html.stylesheet` 引用（默认指向本地 `stylesheet.css`；当 `$website.stylesheet = 1` 时切换到 `https://www.postgresql.org/media/css/docs-complete.css`）。

因此，这个仓库的“frontend_style”实质上是**文档站点的样式体系**，而非产品 UI 样式。

## 2. 关键文件

| 文件 | 作用 |
|---|---|
| `doc/src/sgml/stylesheet.xsl` | 主入口 XSLT，导入 DocBook chunked HTML 模板并定义导航头/尾模板、`html.stylesheet` 选择逻辑 |
| `doc/src/sgml/stylesheet-common.xsl` | 跨输出格式共用的 DocBook 行为定制（TOC 深度、自动编号、`productname`/`returnvalue`/`token` 等元素渲染） |
| `doc/src/sgml/stylesheet-html-common.xsl` | HTML 专用通用定制（由 `stylesheet.xsl` include） |
| `doc/src/sgml/stylesheet-speedup-xhtml.xsl` | 加速 HTML 生成的 DocBook 优化片段 |
| `doc/src/sgml/stylesheet-fo.xsl` | PDF/XSL-FO 输出专用的样式定制 |
| `doc/src/sgml/stylesheet-hh.xsl` | HTML Help 输出的样式定制，设置 `html.stylesheet='stylesheet.css'` |
| `doc/src/sgml/stylesheet.css` | 文档 HTML 的视觉主题：颜色、字体、标题层级、示例框、表格、提示块、媒体对象响应式宽度 |
| `doc/src/sgml/docguide.sgml` | 文档编写指南，说明使用 DocBook XSL stylesheets 以及 `stylesheet.css` 的来源 |

## 3. 架构与约定

- **XSLT 分层**：`stylesheet.xsl` 作为顶层入口，依次 import/include DocBook 官方模板和本地定制层。新增输出格式（如 man、FO、HH）通过各自的 `stylesheet-*.xsl` 复用 `stylesheet-common.xsl`，避免重复。
- **样式与结构分离**：HTML 结构由 DocBook XSLT 生成，视觉表现完全由 `stylesheet.css` 控制。注释明确说明“color scheme similar to www.postgresql.org”，即文档站点遵循 PostgreSQL 官网配色（橙色 `#EC5800` 用于标题，蓝色 `#0066A2` 用于链接）。
- **可切换主题**：通过 XSLT 参数 `website.stylesheet` 决定使用本地 `stylesheet.css` 还是 PostgreSQL 官网托管的 `docs-complete.css`，便于离线构建与在线发布两套风格。
- **响应式策略**：CSS 中使用 `@media (min-width: 800px)` 限制 `.mediaobject` 最大宽度为 75%，实现简单的宽屏适配。
- **设计令牌**：未使用 CSS 变量或设计令牌系统；颜色、字号等硬编码为具体数值（如 `#EC5800`、`#0066A2`、`#666`、`verdana, sans-serif`），属于传统扁平 CSS 写法。

## 4. 约定与约束

- **仅适用于文档输出**：此样式体系只影响 `make -C doc` 生成的 HTML/PDF/Man/HTML Help 文档页面，不影响后端、psql、libpq 或其他任何运行时组件。
- **禁止直接修改 DocBook 官方模板**：所有定制均通过本地 `stylesheet-*.xsl` 覆盖模板或使用 CSS 类（如 `.tip`、`.note`、`.important`、`.caution`、`.warning`）实现，保持与上游 DocBook 样式的解耦。
- **导航头/尾必须通过自定义模板**：`header.navigation` 和 `footer.navigation` 模板被重写以添加 Up/Home/Prev/Next 导航及 tooltip，这是 PostgreSQL 文档导航的标准形态。
- **CSS 类命名来自 DocBook 输出**：样式针对 DocBook 生成的固定 class（如 `.func_table_entry`、`.catalog_table_entry`、`.programlisting`、`.screen`、`.synopsis`、`.option`），新增内容需遵循这些既有 class。
- **无现代前端工具链**：仓库中没有 CSS 预处理器、构建脚本、包管理器或组件库引用；样式维护方式是直接编辑单文件 `stylesheet.css`。