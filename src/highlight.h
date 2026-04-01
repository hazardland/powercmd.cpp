// MODULE: highlight
// Purpose : syntax highlighting — language detection and per-line colorization
// Exports : enum class lang | detect_lang() colorize_inline() colorize_line()
// Depends : common.h

// Language detected from file extension; drives syntax highlight rules.
enum class lang { none, cpp, py, js, json, md, bat, sol, php, go, rust, cs, java, sh, html, yaml, sql };

static lang detect_lang(const std::string& path) {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return lang::none;
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext==".cpp"||ext==".c"||ext==".h"||ext==".hpp"||ext==".cc") return lang::cpp;
    if (ext==".py")                                                   return lang::py;
    if (ext==".js"||ext==".ts"||ext==".jsx"||ext==".tsx")            return lang::js;
    if (ext==".json")                                                 return lang::json;
    if (ext==".md")                                                   return lang::md;
    if (ext==".bat"||ext==".cmd")                                     return lang::bat;
    if (ext==".sol")                                                  return lang::sol;
    if (ext==".php")                                                  return lang::php;
    if (ext==".go")                                                   return lang::go;
    if (ext==".rs")                                                   return lang::rust;
    if (ext==".cs")                                                   return lang::cs;
    if (ext==".java")                                                 return lang::java;
    if (ext==".sh"||ext==".bash")                                     return lang::sh;
    if (ext==".html"||ext==".htm"||ext==".xml"||ext==".svg")         return lang::html;
    if (ext==".yaml"||ext==".yml")                                    return lang::yaml;
    if (ext==".sql")                                                  return lang::sql;
    return lang::none;
}

// Scan a line left-to-right, coloring string literals (yellow), inline comment suffix (gray),
// and any word matching the keywords list (blue). Used for languages with C-style syntax.
static std::string colorize_inline(const std::string& line,
                                   const std::vector<std::string>& kws,
                                   const std::string& comment2 = "",
                                   char comment1 = 0) {
    std::string res;
    size_t i = 0, n = line.size();
    while (i < n) {
        if (!comment2.empty() && i + comment2.size() <= n &&
            line.substr(i, comment2.size()) == comment2) {
            res += GRAY + line.substr(i) + RESET; break;
        }
        if (comment1 && line[i] == comment1) {
            res += GRAY + line.substr(i) + RESET; break;
        }
        if (line[i] == '"' || line[i] == '\'') {
            char q = line[i]; size_t j = i + 1;
            while (j < n && line[j] != q) { if (line[j] == '\\') j++; j++; }
            if (j < n) j++;
            res += YELLOW + line.substr(i, j - i) + RESET;
            i = j; continue;
        }
        if (isalpha((unsigned char)line[i]) || line[i] == '_') {
            size_t j = i;
            while (j < n && (isalnum((unsigned char)line[j]) || line[j] == '_')) j++;
            std::string word = line.substr(i, j - i);
            bool kw = std::find(kws.begin(), kws.end(), word) != kws.end();
            res += kw ? (BLUE + word + RESET) : word;
            i = j; continue;
        }
        res += line[i++];
    }
    return res;
}

// Apply syntax highlighting to one line based on detected language.
static std::string colorize_line(const std::string& line, lang l) {
    if (l == lang::none) return line;
    size_t first = line.find_first_not_of(" \t");
    std::string pfx = (first == std::string::npos) ? "" : line.substr(first);
    std::string pfl = pfx; std::transform(pfl.begin(), pfl.end(), pfl.begin(), ::tolower);

    if (l == lang::md) {
        if (!pfx.empty() && pfx[0] == '#')                              return YELLOW + line + RESET;
        if (pfx.size()>=2 && (pfx[0]=='-'||pfx[0]=='*') && pfx[1]==' ') return BLUE + line + RESET;
        if (pfx.size()>=3 && pfx.substr(0,3)=="```")                    return GREEN + line + RESET;
        return line;
    }
    if (l == lang::bat) {
        if (pfl.size()>=4 && pfl.substr(0,4)=="rem ") return GRAY + line + RESET;
        if (pfx.size()>=2 && pfx.substr(0,2)=="::")   return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            "echo","set","if","else","for","call","goto","exit","mkdir","del",
            "copy","move","pushd","popd","setlocal","endlocal","defined","exist"
        };
        return colorize_inline(line, kw);
    }
    if (l == lang::json) {
        static const std::vector<std::string> kw = {"true","false","null"};
        return colorize_inline(line, kw);
    }
    if (l == lang::cpp) {
        if (pfx.size()>=2 && pfx.substr(0,2)=="//") return GRAY + line + RESET;
        if (pfx.size()>=2 && pfx.substr(0,2)=="/*") return GRAY + line + RESET;
        if (!pfx.empty() && pfx[0]=='#')             return YELLOW + line + RESET;
        static const std::vector<std::string> kw = {
            "auto","bool","break","case","char","class","const","continue","default",
            "delete","do","double","else","enum","explicit","false","float","for",
            "friend","if","inline","int","long","namespace","new","nullptr","operator",
            "override","private","protected","public","return","short","signed","sizeof",
            "static","struct","switch","template","this","throw","true","try","typedef",
            "typename","union","unsigned","using","virtual","void","volatile","while"
        };
        return colorize_inline(line, kw, "//");
    }
    if (l == lang::py) {
        if (!pfx.empty() && pfx[0]=='#') return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            "and","as","assert","async","await","break","class","continue","def","del",
            "elif","else","except","False","finally","for","from","global","if","import",
            "in","is","lambda","None","nonlocal","not","or","pass","raise","return",
            "True","try","while","with","yield"
        };
        return colorize_inline(line, kw, "", '#');
    }
    if (l == lang::js) {
        if (pfx.size()>=2 && pfx.substr(0,2)=="//") return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            "async","await","break","case","catch","class","const","continue","debugger",
            "default","delete","do","else","export","extends","false","finally","for",
            "function","if","import","in","instanceof","let","new","null","of","return",
            "static","super","switch","this","throw","true","try","typeof","undefined",
            "var","void","while","with","yield"
        };
        return colorize_inline(line, kw, "//");
    }
    if (l == lang::html) {
        if (pfx.size()>=4 && pfx.substr(0,4)=="<!--") return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            "html","head","body","title","meta","link","script","style","base",
            "div","span","p","a","br","hr","img","input","button","form","label",
            "select","option","optgroup","textarea","fieldset","legend",
            "h1","h2","h3","h4","h5","h6","ul","ol","li","dl","dt","dd",
            "table","thead","tbody","tfoot","tr","th","td","caption","colgroup","col",
            "header","footer","main","nav","section","article","aside","figure","figcaption",
            "details","summary","dialog","template","slot","canvas","svg","path","rect",
            "circle","ellipse","line","polyline","polygon","text","g","defs","use","symbol",
            "audio","video","source","track","iframe","embed","object","param","picture",
            "pre","code","blockquote","cite","q","abbr","acronym","address","em","strong",
            "small","mark","del","ins","sub","sup","s","u","b","i","bdi","bdo","wbr",
            "noscript","noframes","area","map",
            "class","id","href","src","type","name","value","placeholder","action",
            "method","target","rel","charset","content","lang","dir","style",
            "width","height","alt","title","role","aria","data","for","checked",
            "disabled","readonly","required","multiple","selected","hidden","tabindex",
            "onclick","onload","onchange","oninput","onsubmit","defer","async","crossorigin"
        };
        return colorize_inline(line, kw);
    }
    if (l == lang::php) {
        if (pfx.size()>=2 && pfx.substr(0,2)=="//") return GRAY + line + RESET;
        if (pfx.size()>=2 && pfx.substr(0,2)=="/*") return GRAY + line + RESET;
        if (!pfx.empty() && pfx[0]=='#')             return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            "abstract","array","as","break","callable","case","catch","class","clone",
            "const","continue","declare","default","do","echo","else","elseif","empty",
            "enddeclare","endfor","endforeach","endif","endswitch","endwhile","enum",
            "extends","final","finally","fn","for","foreach","function","global","goto",
            "if","implements","include","include_once","instanceof","insteadof","interface",
            "isset","list","match","namespace","new","null","print","private","protected",
            "public","readonly","require","require_once","return","static","switch","throw",
            "trait","true","false","try","unset","use","var","while","yield","int","float",
            "string","bool","void","never","mixed","self","parent"
        };
        return colorize_inline(line, kw, "//", '#');
    }
    if (l == lang::go) {
        if (pfx.size()>=2 && pfx.substr(0,2)=="//") return GRAY + line + RESET;
        if (pfx.size()>=2 && pfx.substr(0,2)=="/*") return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            "break","case","chan","const","continue","default","defer","else","fallthrough",
            "for","func","go","goto","if","import","interface","map","package","range",
            "return","select","struct","switch","type","var","nil","true","false",
            "int","int8","int16","int32","int64","uint","uint8","uint16","uint32","uint64",
            "uintptr","float32","float64","complex64","complex128","byte","rune","string",
            "bool","error","any","make","new","len","cap","append","copy","delete","close",
            "panic","recover","print","println"
        };
        return colorize_inline(line, kw, "//");
    }
    if (l == lang::rust) {
        if (pfx.size()>=2 && pfx.substr(0,2)=="//") return GRAY + line + RESET;
        if (pfx.size()>=2 && pfx.substr(0,2)=="/*") return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            "as","async","await","break","const","continue","crate","dyn","else","enum",
            "extern","false","fn","for","if","impl","in","let","loop","match","mod","move",
            "mut","pub","ref","return","self","Self","static","struct","super","trait",
            "true","type","unsafe","use","where","while","i8","i16","i32","i64","i128",
            "isize","u8","u16","u32","u64","u128","usize","f32","f64","bool","char","str",
            "String","Vec","Option","Result","Some","None","Ok","Err","Box","Rc","Arc"
        };
        return colorize_inline(line, kw, "//");
    }
    if (l == lang::cs) {
        if (pfx.size()>=2 && pfx.substr(0,2)=="//") return GRAY + line + RESET;
        if (pfx.size()>=2 && pfx.substr(0,2)=="/*") return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            "abstract","as","base","bool","break","byte","case","catch","char","checked",
            "class","const","continue","decimal","default","delegate","do","double","else",
            "enum","event","explicit","extern","false","finally","fixed","float","for",
            "foreach","goto","if","implicit","in","int","interface","internal","is","lock",
            "long","namespace","new","null","object","operator","out","override","params",
            "private","protected","public","readonly","ref","return","sbyte","sealed","short",
            "sizeof","static","string","struct","switch","this","throw","true","try","typeof",
            "uint","ulong","unchecked","unsafe","ushort","using","var","virtual","void",
            "volatile","while","async","await","dynamic","record","init","required","with"
        };
        return colorize_inline(line, kw, "//");
    }
    if (l == lang::java) {
        if (pfx.size()>=2 && pfx.substr(0,2)=="//") return GRAY + line + RESET;
        if (pfx.size()>=2 && pfx.substr(0,2)=="/*") return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            "abstract","assert","boolean","break","byte","case","catch","char","class",
            "const","continue","default","do","double","else","enum","extends","final",
            "finally","float","for","goto","if","implements","import","instanceof","int",
            "interface","long","native","new","null","package","private","protected","public",
            "return","short","static","strictfp","super","switch","synchronized","this",
            "throw","throws","transient","true","false","try","var","void","volatile","while",
            "record","sealed","permits","yield"
        };
        return colorize_inline(line, kw, "//");
    }
    if (l == lang::sh) {
        if (!pfx.empty() && pfx[0]=='#') return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            "if","then","else","elif","fi","for","in","do","done","while","until","case",
            "esac","select","function","return","exit","break","continue","shift","local",
            "readonly","export","unset","source","declare","typeset","eval","exec","trap",
            "wait","read","echo","printf","test","true","false"
        };
        return colorize_inline(line, kw, "", '#');
    }
    if (l == lang::sol) {
        if (pfx.size()>=2 && pfx.substr(0,2)=="//") return GRAY + line + RESET;
        if (pfx.size()>=2 && pfx.substr(0,2)=="/*") return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            "pragma","contract","interface","library","abstract","is","using","import",
            "function","modifier","event","error","struct","enum","mapping","constructor",
            "fallback","receive","returns","return",
            "public","private","internal","external","view","pure","payable","virtual","override",
            "memory","storage","calldata","indexed","anonymous",
            "uint","uint8","uint16","uint32","uint64","uint128","uint256",
            "int","int8","int16","int32","int64","int128","int256",
            "bytes","bytes1","bytes2","bytes4","bytes8","bytes16","bytes32",
            "address","bool","string","fixed","ufixed",
            "if","else","for","while","do","break","continue","new","delete","emit","revert",
            "require","assert","selfdestruct","type","try","catch",
            "true","false","wei","gwei","ether","seconds","minutes","hours","days","weeks"
        };
        return colorize_inline(line, kw, "//");
    }
    if (l == lang::yaml) {
        if (!pfx.empty() && pfx[0] == '#')    return GRAY + line + RESET;
        if (pfx == "---" || pfx == "...")      return YELLOW + line + RESET;
        // key: [value]
        size_t colon = pfx.find(':');
        if (colon != std::string::npos &&
            (colon + 1 >= pfx.size() || pfx[colon + 1] == ' ')) {
            size_t lead = line.find_first_not_of(" \t");
            std::string indent = (lead == std::string::npos) ? "" : line.substr(0, lead);
            std::string result = indent + BLUE + pfx.substr(0, colon) + ":" + RESET;
            std::string after  = pfx.substr(colon + 1);
            if (!after.empty()) {
                size_t vs = after.find_first_not_of(' ');
                std::string sp  = vs == std::string::npos ? after : after.substr(0, vs);
                std::string val = vs == std::string::npos ? "" : after.substr(vs);
                std::string comment;
                size_t hash = val.find(" #");
                if (hash != std::string::npos) { comment = val.substr(hash); val = val.substr(0, hash); }
                while (!val.empty() && val.back() == ' ') val.pop_back();
                static const std::vector<std::string> boolnull = {
                    "true","false","null","yes","no","True","False","Null","Yes","No"
                };
                bool is_bn = std::find(boolnull.begin(), boolnull.end(), val) != boolnull.end();
                if      (val.empty())                           result += sp;
                else if (is_bn)                                 result += sp + BLUE   + val + RESET;
                else if (val[0] == '"' || val[0] == '\'')      result += sp + YELLOW + val + RESET;
                else                                            result += sp + val;
                if (!comment.empty()) result += GRAY + comment + RESET;
            }
            return result;
        }
        // list item: "- "
        if (pfx.size() >= 2 && pfx[0] == '-' && pfx[1] == ' ') {
            size_t dash = line.find('-');
            return line.substr(0, dash) + GREEN + "-" + RESET + line.substr(dash + 1);
        }
        return line;
    }
    if (l == lang::sql) {
        if (pfx.size() >= 2 && pfx.substr(0, 2) == "--") return GRAY + line + RESET;
        static const std::vector<std::string> kw = {
            "select","from","where","join","inner","left","right","full","outer","cross","on",
            "group","by","order","having","limit","offset","fetch","top","distinct","as",
            "insert","into","values","update","set","delete","truncate","merge","using",
            "create","alter","drop","table","view","index","database","schema","trigger","procedure",
            "function","primary","key","foreign","references","constraint","unique","check","default",
            "null","not","and","or","in","exists","between","like","ilike","is","case","when","then",
            "else","end","union","all","except","intersect","with","recursive","over","partition",
            "row_number","rank","dense_rank","count","sum","avg","min","max","cast","convert",
            "begin","commit","rollback","grant","revoke","true","false"
        };
        std::string res;
        size_t i = 0, n = line.size();
        while (i < n) {
            if (i + 1 < n && line[i] == '-' && line[i + 1] == '-') {
                res += GRAY + line.substr(i) + RESET;
                break;
            }
            if (line[i] == '\'' || line[i] == '"') {
                char q = line[i];
                size_t j = i + 1;
                while (j < n) {
                    if (line[j] == q) {
                        if (j + 1 < n && line[j + 1] == q) { j += 2; continue; }
                        j++;
                        break;
                    }
                    j++;
                }
                res += YELLOW + line.substr(i, j - i) + RESET;
                i = j;
                continue;
            }
            if (isalpha((unsigned char)line[i]) || line[i] == '_') {
                size_t j = i;
                while (j < n && (isalnum((unsigned char)line[j]) || line[j] == '_')) j++;
                std::string word = line.substr(i, j - i);
                std::string lower = word;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                bool is_kw = std::find(kw.begin(), kw.end(), lower) != kw.end();
                res += is_kw ? (BLUE + word + RESET) : word;
                i = j;
                continue;
            }
            res += line[i++];
        }
        return res;
    }
    return line;
}
