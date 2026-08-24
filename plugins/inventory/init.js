// 背包系统 (runtime plugin example, JS)
//
// A reusable gameplay module: registers an editor component schema, GM
// commands and an event handler. Plugin state is scoped under
// plugin:inventory:..., so it never collides with scene scripts.

function on_load() {
  // Editor-side schema: adding the "inventory" component in the inspector
  // becomes schema-driven (with defaults) instead of raw JSON.
  Plugin.RegisterComponent("inventory", {
    label: "背包",
    fields: [
      { key: "slots", type: "int", label: "格子数", def: 12, min: 1, max: 64 },
      { key: "maxWeight", type: "number", label: "最大负重", def: 50, min: 0, max: 10000 }
    ]
  });

  // GM command: inv_add <角色> <物品> [数量]
  Plugin.OnCommand("inv_add", function (args) {
    var who = args && args[0] ? String(args[0]) : "player";
    var item = args && args[1] ? String(args[1]) : "gold";
    var count = args && args[2] && typeof args[2] === "number" ? args[2] : 1;
    var key = who + "_" + item;
    var n = Plugin.GetVar(key);
    if (typeof n !== "number") n = 0;
    n += count;
    Plugin.SetVar(key, n);
    Plugin.Log("info", who + " 获得 " + item + " x" + count + "（共 " + n + "）");
    return n;
  }, "inv_add <角色> <物品> [数量]");

  // Server join event (dispatched by GameServer next to on_player_join).
  Plugin.On("player_join", function (clientId) {
    Plugin.Log("info", "玩家 " + clientId + " 加入，背包已就绪");
  });

  // Module API: other plugins (or hosts) can call Plugin.Call("inventory",
  // "addItem", who, item, count) to add items and get the new count back.
  Plugin.Export("addItem", function (who, item, count) {
    var key = who + "_" + item;
    var n = Plugin.GetVar(key);
    if (typeof n !== "number") n = 0;
    n += (count === undefined ? 1 : count);
    Plugin.SetVar(key, n);
    return n;
  });
}
