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
function LC.flow.load(path, key)
  -- key: ��·ʵ����ʶ��ͬһ�����ʵ�����Զ���������
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
    key = key or (def.name or path),  -- ��·ʵ������load/setvar/call ���˶�λ��
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
  -- ͬ key ��ʵ���Ѵ������ظ����أ���������ʵ����
  for _, ex in ipairs(LC.flow.graphs) do
    if ex.key == g.key then return ex end
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
    if (g.key == name or g.name == name) and g.path then
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
LC.flow.evalVal = evalVal  -- export for flowkit/flowgame $var resolution

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
  local a = g.vars[node.var]  -- var is a NAME; evalVal returns the literal string
  if a == nil then a = tonumber(node.var) or node.var end
  local b = evalVal(g, node.value)
  if type(b) == "string" then b = tonumber(b) or b end
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

-- FLOW_DEBUG(name)：返回图变量的快照串 "k=v,k=v"（编辑器运行期显示用）
function FLOW_DEBUG(name)
  for _, g in ipairs(LC.flow.graphs) do
    if g.name == name then
      local parts = {}
      for k, v in pairs(g.vars) do
        parts[#parts + 1] = tostring(k) .. "=" .. tostring(v)
      end
      return table.concat(parts, ",")
    end
  end
  return ""
end

-- ---------------------------------------------------------------------------
-- 节点类型元数据（可视化编辑器扫描 .lua 源发现；与 register 分离，纯声明）：
--   LC.flow.describe("game/toast", "提示文字", "玩法",
--                    { "exec" },                              -- 输出引脚
--                    { { "text", "s" }, { "duration", "n" } }) -- 参数 s文本 n数值 o比较符
-- ---------------------------------------------------------------------------
LC.flow.descriptions = {}
function LC.flow.describe(nodeType, label, category, outs, params)
  LC.flow.descriptions[nodeType] = {
    label = label or nodeType,
    category = category or "玩法",
    outs = outs or { "exec" },
    params = params or {},
  }
end

-- ---------------------------------------------------------------------------
-- 流程核心增强（游戏无关）：
--   flow/random  按概率走 true/false
--   flow/seq     顺序触发全部出链（then1..then3）
--   flow/math    var = var op value（+ - * / min max）
--   flow/call    调用子图（LC.flow.call 触发目标图的指定信号入口）
-- ---------------------------------------------------------------------------
LC.flow.register("flow/random", function(node, g)
  if math.random() < (node.chance or 0.5) then return "true" end
  return "false"
end)

LC.flow.register("flow/seq", function(node, g)
  return { "then1", "then2", "then3" }  -- runFrom 依次深走（未连线的 pin 自动跳过）
end)

LC.flow.register("flow/math", function(node, g)
  local a = g.vars[node.var]
  if type(a) ~= "number" then a = tonumber(a) or 0 end
  local b = tonumber(evalVal(g, node.value)) or 0
  local r = a
  local op = node.op or "+"
  if op == "+" then r = a + b
  elseif op == "-" then r = a - b
  elseif op == "*" then r = a * b
  elseif op == "/" then r = (b ~= 0) and (a / b) or a
  elseif op == "min" then r = math.min(a, b)
  elseif op == "max" then r = math.max(a, b) end
  g.vars[node.var] = r
  return "exec"
end)

LC.flow.register("flow/call", function(node, g)
  LC.flow.call(tostring(node.graph or ""), tostring(node.signal or "start"))
  return "exec"
end)

-- 子图调用：只触发指定图实例内监听该信号的入口（emit 是全图广播，call 是定向）
function LC.flow.call(graphName, signalName)
  for _, g in ipairs(LC.flow.graphs) do
    if g.key == graphName or g.name == graphName then
      for _, e in ipairs(g.def.entry or {}) do
        if e.type == "signal" and e.name == signalName then
          runFrom(g, e.node, "exec", 1)
        end
      end
      return
    end
  end
end

-- 流程核心节点元数据（编辑器自动发现）
LC.flow.describe("flow/setvar", "设置变量", "流程", { "exec" },
                 { { "var", "s" }, { "value", "s" } })
LC.flow.describe("flow/addvar", "变量自增", "流程", { "exec" },
                 { { "var", "s" }, { "value", "n" } })
LC.flow.describe("flow/branch", "条件分支", "流程", { "true", "false" },
                 { { "var", "s" }, { "op", "o" }, { "value", "s" } })
LC.flow.describe("flow/random", "概率分支", "流程", { "true", "false" }, { { "chance", "n" } })
LC.flow.describe("flow/seq", "顺序执行", "流程", { "then1", "then2", "then3" }, {})
LC.flow.describe("flow/math", "变量运算", "流程", { "exec" },
                 { { "var", "s" }, { "op", "o" }, { "value", "n" } })
LC.flow.describe("flow/call", "调用子图", "流程", { "exec" },
                 { { "graph", "s" }, { "signal", "s" } })

-- 流程编排：加载通用玩法节点包 + 主流程图（编辑器保存后热重载即可生效）

-- ---------------------------------------------------------------------------
-- 运行时 API 补充：跨图变量注入（宿主在触发信号前把上下文写进图变量）
-- ---------------------------------------------------------------------------
function LC.flow.setvar(graphName, var, value)
  for _, g in ipairs(LC.flow.graphs) do
    if g.key == graphName or g.name == graphName then
      g.vars[var] = value
      return true
    end
  end
  return false
end
