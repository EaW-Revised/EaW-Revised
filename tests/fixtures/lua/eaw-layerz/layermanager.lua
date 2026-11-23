-- Fixture copy of https://github.com/gruenwaldlk/eaw-layerz (scripts/library/eaw-layerz), used as a test input only.
-- The upstream licence follows and applies to this file.
--
-- MIT License
--
-- Copyright (c) 2021 Lukas Grünwald
--
-- Permission is hereby granted, free of charge, to any person obtaining a copy
-- of this software and associated documentation files (the "Software"), to deal
-- in the Software without restriction, including without limitation the rights
-- to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
-- copies of the Software, and to permit persons to whom the Software is
-- furnished to do so, subject to the following conditions:
--
-- The above copyright notice and this permission notice shall be included in all
-- copies or substantial portions of the Software.
--
-- THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
-- IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
-- FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
-- AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
-- LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
-- OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
-- SOFTWARE.

---@class LayerManager
---*NOTE:* Functions prefixed with a double underscore (`__`) are considered private functions of the module. Call at your own risk.
LayerManager = {
    ---@type Configuration
    CONFIG = require("eaw-layerz/configuration"),
    ---@private
    ---@param self LayerManager
    ---@param game_object GameObject
    ---Hides a given `GameObject` reliably by calling `GameObjectWrapper::Hide(bool b)` twice.
    __hide_game_object = function(self, game_object)
        game_object.Hide(true)
        game_object.Hide(true)
    end,
    ---@private
    ---@param self LayerManager
    ---@param game_object GameObject
    ---@param spawns_fighters boolean
    ---Completely disables a given `GameObject`.
    __disable_game_object = function(self, game_object, spawns_fighters)
        local l_spawns_fighters = spawns_fighters or false
        if l_spawns_fighters then game_object.Set_Garrison_Spawn(false) end
        game_object.Make_Invulnerable(true)
        game_object.Set_Selectable(false)
        game_object.Prevent_All_Fire(true)
    end,
    ---@private
    ---@param self LayerManager
    ---@param game_object GameObject
    ---@param spawns_fighters boolean
    ---Completely re-enables a given `GameObject` that waas previously disabled by `LayerManager#__disable_game_object`
    ---@see LayerManager#__disable_game_object
    __enable_game_object = function(self, game_object, spawns_fighters)
        local l_spawns_fighters = spawns_fighters or false
        game_object.Make_Invulnerable(false)
        game_object.Set_Selectable(true)
        game_object.Prevent_All_Fire(false)
        game_object.Prevent_AI_Usage(false)
        if l_spawns_fighters then game_object.Set_Garrison_Spawn(true) end
    end,
    ---@private
    ---@param self LayerManager
    ---@param game_object GameObject
    __randomise_z_axis_coordinate = function(self, game_object)
        local l_position = game_object.Get_Position()
        local l_min, l_max = self:__resolve_layer(game_object)
        if l_min == l_max then return end
        local l_x0, l_x1, l_x2 = l_position.Get_XYZ()
        l_position = Create_Position(l_x0, l_x1, l_x2 + GameRandom(l_min, l_max))
        game_object.Teleport(l_position)
    end,
    ---@private
    ---@param self LayerManager
    ---@param game_object GameObject
    ---@return integer minimum
    ---@return integer maximum
    ---Returns the possible bones.
    __resolve_layer = function(self, game_object)
        for _, l_category_map in pairs(self.CONFIG.CATEGORY_MAP) do
            if game_object.Is_Category(l_category_map.ID) then
                return l_category_map.get_range()
            end
        end
        return self.CONFIG.CATEGORY_MAP.default()
    end,
    ---@param self LayerManager
    ---@param game_object GameObject
    ---@param spawns_fighters boolean
    ---Moves the units to a randomised Z-coordinate based on its category masks and the configurations made.
    ---@see Configuration
    update_unit_layer = function(self, game_object, spawns_fighters)
        if not spawns_fighters then spawns_fighters = false end
        self:__disable_game_object(game_object, spawns_fighters)
        game_object.Cancel_Hyperspace()
        self:__hide_game_object(game_object)
        self:__randomise_z_axis_coordinate(game_object)
        game_object.Cinematic_Hyperspace_In(1)
        self:__enable_game_object(game_object, spawns_fighters)
    end
}

return LayerManager
