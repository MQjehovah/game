-- 艾希 被动「冰霜射击」：普攻让目标减速 20%，持续 1.2 秒。
local M = {}
function M.on_hit(p, target)
    if target == nil or target.dead then return end
    p.applyStatus(target, "slow", 1.2, 0.2)
end
return M
