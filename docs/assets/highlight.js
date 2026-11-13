/* Syntax highlighting for <pre class="lang-*"><code> blocks. No dependencies.
   Pages work without it (the code is just plain text). */
(function () {
  "use strict";

  function words(s) {
    var o = {};
    s.split(" ").forEach(function (w) { o[w] = 1; });
    return o;
  }

  var NUM = /\b(?:0[xX][0-9a-fA-F']+|\d[\d']*\.?\d*(?:[eE][+-]?\d+)?[fFuUlL]*)/;
  var DQ = /"(?:\\.|[^"\\\n])*"/;

  // Each language is an ordered list of [token class, RegExp]; the first rule
  // that matches at the earliest position wins. Class "w" is a bare word that
  // is looked up in kw/ty afterwards.
  var LANGS = {
    cpp: {
      rules: [
        ["c", /\/\/[^\n]*|\/\*[\s\S]*?\*\//],
        ["p", /^[ \t]*#[ \t]*\w+(?:[ \t]*<[^>\n]*>)?/],
        ["s", /R"\([\s\S]*?\)"|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'/],
        ["n", NUM],
        ["w", /[A-Za-z_]\w*/]
      ],
      kw: words("alignas alignof auto bool break case catch char class const constexpr consteval constinit const_cast continue decltype default delete do double dynamic_cast else enum explicit export extern false final float for friend goto if inline int long mutable namespace new noexcept nullptr operator override private protected public register reinterpret_cast return short signed sizeof static static_assert static_cast struct switch template this thread_local throw true try typedef typeid typename union unsigned using virtual void volatile while"),
      ty: words("int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t size_t string string_view vector array map unordered_map optional unique_ptr shared_ptr span function"),
      caps: true
    },
    lua: {
      rules: [
        ["c", /--\[=*\[[\s\S]*?\]=*\]|--[^\n]*/],
        ["s", /\[=*\[[\s\S]*?\]=*\]|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'/],
        ["n", NUM],
        ["w", /[A-Za-z_]\w*/]
      ],
      kw: words("and break do else elseif end false for function goto if in local nil not or repeat return then true until while"),
      ty: {},
      caps: true
    },
    json: {
      rules: [
        ["k", /"(?:\\.|[^"\\\n])*"(?=\s*:)/],
        ["s", DQ],
        ["n", /-?\d+\.?\d*(?:[eE][+-]?\d+)?/],
        ["w", /true|false|null/]
      ],
      kw: words("true false null"),
      ty: {}
    },
    shell: {
      rules: [
        ["c", /(?:^|\s)#[^\n]*/],
        ["s", /"(?:\\.|[^"\\])*"|'[^']*'/],
        ["v", /\$\{[^}\n]*\}|\$[A-Za-z_]\w*|%[A-Za-z_]\w*%/],
        ["o", /(?:^|\s)--?[A-Za-z][\w-]*/],
        ["n", /\b\d+\.?\d*\b/],
        ["w", /[A-Za-z_][\w-]*/]
      ],
      kw: words("if then else elif fi for while do done case esac in function return exit export cd set unset"),
      ty: {}
    },
    cmake: {
      rules: [
        ["c", /#[^\n]*/],
        ["s", /"(?:\\.|[^"\\])*"/],
        ["v", /\$\{[^}\n]*\}/],
        ["n", /\b\d+\.?\d*\b/],
        ["w", /[A-Za-z_]\w*/]
      ],
      kw: words("if else elseif endif foreach endforeach while endwhile function endfunction macro endmacro return set option project add_executable add_library target_link_libraries target_include_directories find_package include cmake_minimum_required"),
      ty: {}
    },
    yaml: {
      rules: [
        ["c", /#[^\n]*/],
        ["k", /^[ \t-]*[\w.\/-]+(?=:)/],
        ["s", /"(?:\\.|[^"\\\n])*"|'[^'\n]*'/],
        ["n", /\b\d+\.?\d*\b/],
        ["w", /true|false|null/]
      ],
      kw: words("true false null"),
      ty: {}
    },
    ini: {
      rules: [
        ["c", /[;#][^\n]*/],
        ["p", /^\[[^\]\n]*\]/],
        ["k", /^[ \t]*[\w.-]+(?=[ \t]*=)/],
        ["s", DQ],
        ["n", /\b\d+\.?\d*\b/]
      ],
      kw: {},
      ty: {}
    }
  };
  LANGS.bash = LANGS.shell;
  LANGS.sh = LANGS.shell;
  LANGS.hpp = LANGS.cpp;

  function esc(s) {
    return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  }
  function span(cls, text) {
    return '<span class="tok-' + cls + '">' + esc(text) + "</span>";
  }

  function compile(lang) {
    if (lang.re) return lang;
    // Wrap each rule in a capture group; count each rule's inner groups so we
    // know which outer group matched.
    lang.idx = [];
    var g = 1, parts = [];
    lang.rules.forEach(function (r) {
      lang.idx.push(g);
      g += 1 + (new RegExp(r[1].source + "|").exec("").length - 1);
      parts.push("(" + r[1].source + ")");
    });
    lang.re = new RegExp(parts.join("|"), "gm");
    return lang;
  }

  function highlight(src, lang) {
    compile(lang);
    var out = "", last = 0, m;
    lang.re.lastIndex = 0;
    while ((m = lang.re.exec(src)) !== null) {
      if (m[0] === "") { lang.re.lastIndex++; continue; }
      var rule = -1;
      for (var i = 0; i < lang.idx.length; i++) {
        if (m[lang.idx[i]] !== undefined) { rule = i; break; }
      }
      if (rule < 0) continue;
      var cls = lang.rules[rule][0], text = m[0];
      out += esc(src.slice(last, m.index));
      last = m.index + text.length;
      if (cls === "w") {
        if (lang.kw[text]) cls = "kw";
        else if (lang.ty[text]) cls = "ty";
        else if (src.charAt(last) === "(") cls = "fn";
        else if (lang.caps && /^[A-Z]/.test(text)) cls = "ty";
        else { out += esc(text); continue; }
      } else if (cls === "o" || cls === "c") {
        // Rules that may start with a leading space: keep the space unstyled.
        var lead = text.match(/^\s*/)[0];
        out += esc(lead) + span(cls, text.slice(lead.length));
        continue;
      }
      out += span(cls, text);
    }
    return out + esc(src.slice(last));
  }

  function run() {
    var codes = document.querySelectorAll('pre[class^="lang-"] > code');
    for (var i = 0; i < codes.length; i++) {
      var code = codes[i];
      var lang = LANGS[code.parentNode.className.replace(/^lang-/, "").split(" ")[0]];
      if (!lang) continue;
      try {
        code.innerHTML = highlight(code.textContent, lang);
      } catch (e) { /* leave the plain text */ }
    }
  }

  if (typeof document !== "undefined") {
    if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", run);
    else run();
  }
  if (typeof module !== "undefined") module.exports = { highlight: highlight, LANGS: LANGS };
})();
