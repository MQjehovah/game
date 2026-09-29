-- lc_json.lua
-- 极简 JSON 编解码（存读档用）

function jsonEncode(v)
  local t = type(v)
  if t == "table" then
    local isArr = #v > 0
    local parts = {}
    if isArr then
      for _, item in ipairs(v) do parts[#parts + 1] = jsonEncode(item) end
      return "[" .. table.concat(parts, ",") .. "]"
    end
    for k, item in pairs(v) do
      parts[#parts + 1] = '"' .. k .. '":' .. jsonEncode(item)
    end
    return "{" .. table.concat(parts, ",") .. "}"
  elseif t == "number" then
    return string.format("%.4g", v)
  elseif t == "string" then
    return '"' .. v .. '"'
  elseif t == "boolean" then
    return v and "true" or "false"
  end
  return "null"
end

function jsonDecode(s)
  local pos = 1
  local function skipWs()
    while pos <= #s do
      local c = string.sub(s, pos, pos)
      if c == " " or c == "\t" or c == "\n" or c == "\r" then pos = pos + 1
      else break end
    end
  end
  local parseValue
  local function parseString()
    pos = pos + 1
    local out = {}
    while pos <= #s do
      local c = string.sub(s, pos, pos)
      if c == '"' then pos = pos + 1 return table.concat(out) end
      if c == "\\" then
        pos = pos + 1
        local e = string.sub(s, pos, pos)
        if e == "n" then out[#out + 1] = "\n"
        elseif e == "t" then out[#out + 1] = "\t"
        else out[#out + 1] = e end
        pos = pos + 1
      else
        out[#out + 1] = c
        pos = pos + 1
      end
    end
    return table.concat(out)
  end
  local function parseNumber()
    local start = pos
    while pos <= #s do
      local c = string.sub(s, pos, pos)
      if c == "-" or c == "+" or c == "." or (c >= "0" and c <= "9")
         or c == "e" or c == "E" then pos = pos + 1
      else break end
    end
    return tonumber(string.sub(s, start, pos - 1)) or 0
  end
  parseValue = function()
    skipWs()
    local c = string.sub(s, pos, pos)
    if c == "{" then
      pos = pos + 1
      local obj = {}
      skipWs()
      if string.sub(s, pos, pos) == "}" then pos = pos + 1 return obj end
      while true do
        skipWs()
        local key = parseString()
        skipWs()
        pos = pos + 1
        obj[key] = parseValue()
        skipWs()
        local sep = string.sub(s, pos, pos)
        pos = pos + 1
        if sep == "}" then return obj end
      end
    elseif c == "[" then
      pos = pos + 1
      local arr = {}
      skipWs()
      if string.sub(s, pos, pos) == "]" then pos = pos + 1 return arr end
      while true do
        arr[#arr + 1] = parseValue()
        skipWs()
        local sep = string.sub(s, pos, pos)
        pos = pos + 1
        if sep == "]" then return arr end
      end
    elseif c == '"' then
      return parseString()
    elseif string.sub(s, pos, pos + 3) == "true" then
      pos = pos + 4 return true
    elseif string.sub(s, pos, pos + 4) == "false" then
      pos = pos + 5 return false
    elseif string.sub(s, pos, pos + 3) == "null" then
      pos = pos + 4 return nil
    else
      return parseNumber()
    end
  end
  return parseValue()
end
