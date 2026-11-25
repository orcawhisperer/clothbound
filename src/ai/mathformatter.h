#pragma once

#include <QString>

// Stateless offline LaTeX-to-HTML rendering engine and nonce-based
// Markdown/RichText sanitizer shared by Companion and tests. Pure C++/Qt
// (no QObject dependency).
namespace MathFormatter {

// Strips dangerous raw HTML tags and Markdown image embeds, renders genuine
// offline LaTeX math ($...$, $$...$$, \(...\), \[...\]) into styled HTML
// spans/blocks, and escapes all remaining raw HTML outside code fences/spans.
QString cleanMarkdown(const QString &text);

// Returns true iff `text` contains at least one genuine LaTeX math expression
// outside code blocks/spans (ignoring currency spans such as "$5 to $10" and
// forged raw HTML wrappers).
bool hasMath(const QString &text);

// Renders offline LaTeX math expressions inside Markdown using unforgeable
// per-call random nonces while escaping raw HTML outside code fences/spans.
QString formatLatexInMarkdown(const QString &text);

// Converts untrusted Markdown + LaTeX into safe Qt RichText HTML for QML
// Text/TextEdit, stripping images, active links, and unsafe attributes, and
// rendering display math blocks with a sanitized `mathBlockBg` tint.
QString formatDisplayHtml(const QString &text, const QString &mathBlockBg = {});

}  // namespace MathFormatter
