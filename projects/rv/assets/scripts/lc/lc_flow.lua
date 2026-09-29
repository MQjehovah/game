-- ===========================================================================
-- lc_flow.lua — 数据驱动流程图运行时（可复用，与具体游戏解耦）
--
-- 图格式（JSON，也是可视化编辑器的文件格式）：
-- {
--   "name":  "raid_wave",
--   "vars":  { "wave": 0 },                       -- 图实例变量
--   "entry": [ { "type": "signal", "name": "raid_start", "node": 1 },
--              { "type": "timer",  "interval": 4, "node": 1 },
--              { "type": "tick",   "node": 7 } ],
--   "nodes": [ { "id": 1, "type": "game/horde", "count": 3, "at": "ring:12" },
--              { "id": 2, "type": "flow/branch", "var": "wave", "op": "<", "value": 5 } ],
--   "links": [ { "from": 1, "out": "done", "to": 2, "in": "exec" },
--              { "from": 2, "out": "true",  "to": 3, "in": "exec" } ],
--   "editor": { "positions": { "1": [120, 80] } }   -- 编辑器布局，运行时忽略
-- }
--
-- 执行语义：入口触发（信号/计时器/每帧）→ 从绑定节点沿 exec 链深走 →
-- 节点处理器由宿主游戏通过 LC.flow.register(type, fn) 注册（游戏节点包），
-- 流程核心只管调度/变量/分支。参数字符串 "$var" 取图变量，否则字面量。
-- ===========================================================================

LC = LC or {}
LC.flow = {
  graphs = {},     -- 已实例化的图
  types = {},      -- 节点类型注册表：type -> handler(node, g) -> outPin 名
  maxDepth = 48,   -- exec 链深度上限（防环）
}

-- 节点类型注册（宿主游戏把玩法动作注册进来）
function LC.flow.register(nodeType, handler)
  LC.flow.types[nodeType] = handler
end

-- 读图并实例化（同一文件可多实例；name 相同则跳过）
function LC.flow.load(path)
  local text = ReadText(path)
  if not text or text == "" then
    print(string.format("flow: %s not found", path))
    return nil
  end
  local def = jsonDecode(text)
  if type(def) ~= "table" or type(def.nodes) ~= "table" then
    return nil
  end
  local g = {
    def = def,
    name = def.name or path,
    path = path,   -- 热重载用（LC.flow.reload 按它重读文件）
    vars = {},
    acc = {},      -- 入口累积器（timer/tick）
    nodeById = {},
  }
  for _, n in ipairs(def.nodes or {}) do
    g.nodeById[n.id] = n
  end
  for k, v in pairs(def.vars or {}) do g.vars[k] = v end
  for _, e in ipairs(def.entry or {}) do
    if e.type == "timer" then g.acc["timer:" .. tostring(e.node) .. ":" .. tostring(e.interval)] = 0 end
    if e.type == "tick" then g.acc["tick:" .. tostring(e.node)] = 0 end
  end
  LC.flow.graphs[#LC.flow.graphs + 1] = g
  return g
end

-- ---------------------------------------------------------------------------
-- 可视化编辑器桥
-- FLOW_DEBUG_NODE（全局字符串 "图名:节点id"）：runFrom 每次触发更新，编辑器
--   读取后在画布上高亮"正在执行"的节点（运行期调试视图）。
-- FLOW_RELOAD(name)（全局函数）：编辑器保存后调用，按 g.path 重读文件替换
--   def/nodeById，但保留 vars/acc —— 运行状态不丢，可边玩边调。
-- ---------------------------------------------------------------------------
function LC.flow.reload(name)
  for _, g in ipairs(LC.flow.graphs) do
    if g.name == name and g.path then
      local text = ReadText(g.path)
      if not text or text == "" then return false end
      local def = jsonDecode(text)
      if type(def) ~= "table" or type(def.nodes) ~= "table" then return false end
      g.def = def
      g.nodeById = {}
      for _, n in ipairs(def.nodes or {}) do g.nodeById[n.id] = n end
      -- 新文件里新增的 timer/tick 累积器补零（旧的保留，删掉的留残值无害）
      for _, e in ipairs(def.entry or {}) do
        if e.type == "timer" then
          local k = "timer:" .. tostring(e.node) .. ":" .. tostring(e.interval)
          g.acc[k] = g.acc[k] or 0
        end
      end
      return true
    end
  end
  return false
end

-- 参数求值："$var" -> 图变量；其余原样
local function evalVal(g, v)
  if type(v) == "string" and string.sub(v, 1, 1) == "$" then
    return g.vars[string.sub(v, 2)]
  end
  return v
end

local function compare(a, op, b)
  if op == "<" then return a < b
  elseif op == "<=" then return a <= b
  elseif op == ">" then return a > b
  elseif op == ">=" then return a >= b
  elseif op == "==" then return a == b
  elseif op == "~=" then return a ~= b end
  return false
end

-- 从节点沿指定 out pin 深走 exec 链
local function runFrom(g, nodeId, outPin, depth)
  if depth > LC.flow.maxDepth then return end
  -- 调试桥：记录"即将执行的节点"（编辑器高亮用）
  _G.FLOW_DEBUG_NODE = tostring(g.name) .. ":" .. tostring(nodeId)
  for _, lk in ipairs(g.def.links or {}) do
    if lk.from == nodeId and (lk.out or "exec") == outPin then
      local node = g.nodeById[lk.to]
      if node == nil then return end
      local handler = LC.flow.types[node.type]
      if handler == nil then
        if NEON_LOG_WARN then NEON_LOG_WARN("flow: unknown node type '%s'", tostring(node.type)) end
        return
      end
      local outs = handler(node, g)
      if type(outs) == "string" then
        runFrom(g, node.id, outs, depth + 1)
      elseif type(outs) == "table" then
        for _, pin in ipairs(outs) do runFrom(g, node.id, pin, depth + 1) end
      end
      return  -- 多 out 只跟第一个匹配 pin 的链（条件节点双出各自成链）
    end
  end
end

-- 信号：触发所有图中监听该信号的入口
function LC.flow.emit(name)
  for _, g in ipairs(LC.flow.graphs) do
    for _, e in ipairs(g.def.entry or {}) do
      if e.type == "signal" and e.name == name then
        runFrom(g, e.node, "exec", 1)
      end
    end
  end
end

-- 每帧：timer / tick 入口
function LC.flow.tick(dt)
  for _, g in ipairs(LC.flow.graphs) do
    for _, e in ipairs(g.def.entry or {}) do
      local gated = e.gate and not g.vars[e.gate]
      if e.type == "timer" and not gated then
        local key = "timer:" .. tostring(e.node) .. ":" .. tostring(e.interval)
        g.acc[key] = (g.acc[key] or 0) + dt
        if g.acc[key] >= (e.interval or 1) then
          g.acc[key] = 0
          runFrom(g, e.node, "exec", 1)
        end
      elseif e.type == "tick" and not gated then
        runFrom(g, e.node, "exec", 1)
      end
    end
  end
end

-- ---------------------------------------------------------------------------
-- 内置流程节点（纯流程控制；玩法节点由游戏注册）
-- ---------------------------------------------------------------------------
LC.flow.register("flow/branch", function(node, g)
  local a = evalVal(g, node.var)
  local b = evalVal(g, node.value)
  return compare(a, node.op or "==", b) and "true" or "false"
end)

LC.flow.register("flow/setvar", function(node, g)
  g.vars[node.var] = evalVal(g, node.value)
  return "exec"
end)

LC.flow.register("flow/addvar", function(node, g)
  local cur = g.vars[node.var] or 0
  g.vars[node.var] = cur + (evalVal(g, node.value) or 1)
  return "exec"
end)

-- ---------------------------------------------------------------------------
-- ȫ���ţ��� C++ �� IScriptHost ֱ�� Call/GetGlobal��Lua ������ LC.flow.*��
-- ---------------------------------------------------------------------------
function FLOW_RELOAD(name) return LC.flow.reload(name) end
