-- 本地素材库 (editor plugin example, Lua)
--
-- Registers an asset source (素材市场): the vault/ folder inside the plugin
-- is exposed in the asset panel; 导入 copies an entry into the project's
-- current asset browse dir. Point the vault at a shared network folder to
-- share a team-wide asset library.

local VAULT = "plugins/examples/asset_vault/vault"

function on_load()
  NeonEditor.assetSource(
    "vault",
    "本地素材库",
    function()
      local files = NeonEditor.listDir(VAULT)
      local out = {}
      for i = 1, #files do
        local f = files[i]
        local lower = string.lower(f)
        local kind = "asset"
        if lower:find("%.obj$") or lower:find("%.gltf$") then kind = "model" end
        if lower:find("%.png$") or lower:find("%.jpg$") or lower:find("%.jpeg$") then
          kind = "texture"
        end
        out[i] = { name = f, type = kind, path = f }
      end
      return out
    end,
    function(path)
      local rel = NeonEditor.importAsset(path)
      if rel == "" then
        NeonEditor.log("导入失败: " .. path)
        return ""
      end
      return rel
    end
  )
end
