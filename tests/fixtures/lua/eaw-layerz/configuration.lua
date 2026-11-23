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

---@class Configuration
Configuration = {
    ---@type table<string,function>[]
    ---A map of anonymous objects that define the mapping between the category mask and (a subset of) bones of the `LAYER_DUMMY_TYPE`.
    CATEGORY_MAP = {
        {
            ---@type string
            ID = "Corvette",
            ---@return integer l_min minimum offset
            ---@return integer l_max maximum offset
            ---**TODO:** Update to desired range.
            get_range = function() return -40, 40 end
        },
        {
            -- @type string
            ID = "Frigate",
            ---@return integer l_min minimum offset
            ---@return integer l_max maximum offset
            ---**TODO:** Update to desired range.
            get_range = function() return 0, 0 end

        },
        {
            -- @type string
            ID = "Capital",
            ---@return integer l_min minimum offset
            ---@return integer l_max maximum offset
            ---**TODO:** Update to desired range.
            get_range = function() return 0, 0 end

        },
        ---@return integer l_min minimum offset
        ---@return integer l_max maximum offset
        ---**TODO:** Update to desired range.
        default = function() return 0, 0 end
    }
}

return Configuration
