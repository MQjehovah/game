-- 提莫 被动「剧毒射击」：普攻附加中毒（3 秒，每秒 4 点魔法伤害）。
local M = {}
function M.on_hit(p, target)
    if target == nil or target.dead then return end
    p.applyStatus(target, "poison", 3, 4)
end
return M
