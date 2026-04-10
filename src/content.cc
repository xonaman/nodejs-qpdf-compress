#include "content.h"

#include <cctype>
#include <string>

#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

// ---------------------------------------------------------------------------
// Content stream coalescing — merge multiple content streams per page into one
// ---------------------------------------------------------------------------

void coalesceContentStreams(QPDF &qpdf) {
  for (auto &page : QPDFPageDocumentHelper(qpdf).getAllPages()) {
    auto pageObj = page.getObjectHandle();
    auto contents = pageObj.getKey("/Contents");

    // only coalesce if there are multiple content streams (array)
    if (contents.isArray() && contents.getArrayNItems() > 1) {
      page.coalesceContentStreams();
    }
  }
}

// ---------------------------------------------------------------------------
// Content stream minification — normalize whitespace and number formatting
// to reduce content stream size before Flate compression
// ---------------------------------------------------------------------------

// trims a numeric string: remove trailing zeros after decimal point,
// remove the decimal point if it becomes the last char,
// and strip a leading zero for values between -1 and 1.
static std::string trimNumber(const std::string &s) {
  // only process strings that look like decimal numbers
  if (s.find('.') == std::string::npos)
    return s;

  std::string result = s;

  // strip trailing zeros after decimal point
  size_t dot = result.find('.');
  if (dot != std::string::npos) {
    size_t last = result.size() - 1;
    while (last > dot && result[last] == '0')
      --last;
    if (last == dot)
      result.erase(dot); // remove the dot too (e.g. "1." → "1")
    else
      result.erase(last + 1);
  }

  // if trimming left an empty string or bare sign, the value was zero
  if (result.empty() || result == "-")
    return "0";

  // strip leading zero for values like "0.5" → ".5" or "-0.5" → "-.5"
  if (result.size() >= 2 && result[0] == '0' && result[1] == '.')
    result.erase(0, 1);
  else if (result.size() >= 3 && result[0] == '-' && result[1] == '0' &&
           result[2] == '.')
    result.erase(1, 1);

  return result;
}

void minifyContentStreams(QPDF &qpdf) {
  for (auto &page : QPDFPageDocumentHelper(qpdf).getAllPages()) {
    auto pageObj = page.getObjectHandle();
    auto contents = pageObj.getKey("/Contents");

    if (!contents.isStream())
      continue;

    std::string raw;
    try {
      auto buf = contents.getStreamData(qpdf_dl_generalized);
      raw.assign(reinterpret_cast<const char *>(buf->getBuffer()),
                 buf->getSize());
    } catch (...) {
      continue;
    }

    // tokenize preserving string literals and hex strings intact
    std::string minified;
    minified.reserve(raw.size());
    bool needSpace = false;
    size_t pos = 0;

    while (pos < raw.size()) {
      char ch = raw[pos];

      // skip whitespace
      if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
        if (!minified.empty())
          needSpace = true;
        ++pos;
        continue;
      }

      // comments — skip to end of line
      if (ch == '%') {
        while (pos < raw.size() && raw[pos] != '\n')
          ++pos;
        continue;
      }

      // literal string — copy verbatim
      if (ch == '(') {
        if (needSpace) {
          minified += ' ';
          needSpace = false;
        }
        int depth = 1;
        minified += '(';
        ++pos;
        while (pos < raw.size() && depth > 0) {
          if (raw[pos] == '\\') {
            minified += raw[pos++];
            if (pos < raw.size())
              minified += raw[pos++];
          } else {
            if (raw[pos] == '(')
              ++depth;
            else if (raw[pos] == ')')
              --depth;
            minified += raw[pos++];
          }
        }
        needSpace = true;
        continue;
      }

      // hex string — copy verbatim
      if (ch == '<' && pos + 1 < raw.size() && raw[pos + 1] != '<') {
        if (needSpace) {
          minified += ' ';
          needSpace = false;
        }
        minified += '<';
        ++pos;
        while (pos < raw.size() && raw[pos] != '>') {
          if (!std::isspace(static_cast<unsigned char>(raw[pos])))
            minified += raw[pos];
          ++pos;
        }
        if (pos < raw.size()) {
          minified += '>';
          ++pos;
        }
        needSpace = true;
        continue;
      }

      // dict delimiters << >> — self-delimiting, no space needed around them
      if (ch == '<' && pos + 1 < raw.size() && raw[pos + 1] == '<') {
        if (needSpace) {
          minified += ' ';
          needSpace = false;
        }
        minified += "<<";
        pos += 2;
        continue;
      }
      if (ch == '>' && pos + 1 < raw.size() && raw[pos + 1] == '>') {
        minified += ">>";
        pos += 2;
        needSpace = true;
        continue;
      }

      // array delimiters — self-delimiting
      if (ch == '[' || ch == ']') {
        if (needSpace && ch == '[') {
          minified += ' ';
          needSpace = false;
        }
        minified += ch;
        ++pos;
        if (ch == ']')
          needSpace = true;
        continue;
      }

      // name — starts with /
      if (ch == '/') {
        if (needSpace) {
          minified += ' ';
          needSpace = false;
        }
        size_t start = pos;
        ++pos;
        while (pos < raw.size() &&
               !std::isspace(static_cast<unsigned char>(raw[pos])) &&
               raw[pos] != '/' && raw[pos] != '[' && raw[pos] != ']' &&
               raw[pos] != '<' && raw[pos] != '>' && raw[pos] != '(' &&
               raw[pos] != ')')
          ++pos;
        minified.append(raw, start, pos - start);
        needSpace = true;
        continue;
      }

      // regular token (number, operator)
      {
        if (needSpace) {
          minified += ' ';
          needSpace = false;
        }
        size_t start = pos;
        while (pos < raw.size() &&
               !std::isspace(static_cast<unsigned char>(raw[pos])) &&
               raw[pos] != '/' && raw[pos] != '[' && raw[pos] != ']' &&
               raw[pos] != '<' && raw[pos] != '>' && raw[pos] != '(' &&
               raw[pos] != ')')
          ++pos;

        std::string token(raw, start, pos - start);

        // trim numeric formatting
        if (!token.empty() &&
            (token[0] == '-' || token[0] == '+' || token[0] == '.' ||
             (token[0] >= '0' && token[0] <= '9'))) {
          token = trimNumber(token);
        }

        minified += token;
        needSpace = true;

        // handle inline image data (BI <key-value pairs> ID <binary> EI)
        // the binary data after ID can contain any byte — copy verbatim
        if (token == "ID") {
          // copy the required single whitespace delimiter after ID
          if (pos < raw.size())
            minified += raw[pos++];
          // copy binary data verbatim until the EI end marker:
          // whitespace + "EI" + (whitespace or end-of-stream)
          // binary image data can contain byte sequences that match this
          // pattern, so we validate that what follows EI looks like valid
          // content stream syntax (not more binary data)
          bool foundEI = false;
          while (pos + 2 < raw.size()) {
            if (std::isspace(static_cast<unsigned char>(raw[pos])) &&
                raw[pos + 1] == 'E' && raw[pos + 2] == 'I' &&
                (pos + 3 >= raw.size() ||
                 std::isspace(static_cast<unsigned char>(raw[pos + 3])))) {
              // validate: after EI + whitespace, the next non-whitespace byte
              // must be a valid content stream token start character, not
              // a continuation of binary image data
              size_t check = pos + 3;
              while (check < raw.size() &&
                     std::isspace(static_cast<unsigned char>(raw[check])))
                check++;
              bool valid = (check >= raw.size());
              if (!valid) {
                unsigned char c = static_cast<unsigned char>(raw[check]);
                // valid token start: letter (operator), digit/sign (number),
                // ( (string), < (hex/dict), [ (array), / (name), % (comment)
                valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '+' || c == '-' ||
                        c == '.' || c == '(' || c == '<' || c == '[' ||
                        c == '/' || c == '%';
              }
              if (valid) {
                minified += raw[pos]; // whitespace before EI
                minified += "EI";
                pos += 3;
                foundEI = true;
                break;
              }
              // false EI match inside binary data — continue scanning
            }
            minified += raw[pos++];
          }
          if (!foundEI) {
            // malformed stream — copy remaining bytes verbatim
            while (pos < raw.size())
              minified += raw[pos++];
          }
        }
      }
    }

    // only replace if we actually reduced the size
    if (minified.size() >= raw.size())
      continue;

    contents.replaceStreamData(minified, QPDFObjectHandle::newNull(),
                               QPDFObjectHandle::newNull());
  }
}
