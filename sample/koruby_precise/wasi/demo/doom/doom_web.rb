WAD_PATH = "/doom/doom1.wad"
module Doom; module Platform; class GosuWindow; end; end; end
# frozen_string_literal: true

module Doom
  VERSION = '0.9.0'
end

# frozen_string_literal: true

module Doom
  module Wad
    class Reader
      IWAD = 'IWAD'
      PWAD = 'PWAD'

      DirectoryEntry = Struct.new(:offset, :size, :name)

      attr_reader :type, :num_lumps, :directory

      def initialize(path)
        @file = File.open(path, 'rb')
        read_header
        read_directory
      end

      def find_lump(name)
        @directory.find { |entry| entry.name == name.upcase }
      end

      def read_lump(name)
        return @lump_cache[name] if @lump_cache.key?(name)

        entry = find_lump(name)
        return nil unless entry

        @file.seek(entry.offset)
        data = @file.read(entry.size)
        @lump_cache[name] = data
        data
      end

      def read_lump_at(entry)
        @file.seek(entry.offset)
        @file.read(entry.size)
      end

      def lumps_between(start_marker, end_marker)
        start_idx = @directory.index { |e| e.name == start_marker }
        end_idx = @directory.index { |e| e.name == end_marker }
        return [] unless start_idx && end_idx

        @directory[start_idx + 1...end_idx]
      end

      def pwad?
        @type == PWAD
      end

      def iwad?
        @type == IWAD
      end

      # Merge a PWAD on top of this IWAD.
      # PWAD lumps override IWAD lumps with the same name.
      # Map lumps (between map markers) are replaced as a group.
      def merge_pwad(pwad)
        raise Error, "Can only merge a PWAD" unless pwad.pwad?
        @lump_cache.clear

        pwad.directory.each do |pwad_entry|
          # Check if this lump already exists in the IWAD
          existing_idx = @directory.index { |e| e.name == pwad_entry.name }
          if existing_idx
            # Replace it -- but we need to read from the PWAD file
            @directory[existing_idx] = pwad_entry
          else
            # Append new lump
            @directory << pwad_entry
          end
        end

        # Keep the PWAD file open for reading its lumps
        @pwad_files ||= []
        @pwad_files << pwad
        @num_lumps = @directory.size
      end

      # Override read_lump_at to check if entry belongs to a PWAD file
      def read_lump_at(entry)
        # Try PWAD files first (they may own this entry)
        (@pwad_files || []).each do |pwad|
          if pwad.directory.include?(entry)
            return pwad.read_lump_own(entry)
          end
        end
        @file.seek(entry.offset)
        @file.read(entry.size)
      end

      # Read a lump that belongs to this WAD's own file
      def read_lump_own(entry)
        @file.seek(entry.offset)
        @file.read(entry.size)
      end

      def close
        @file.close
        (@pwad_files || []).each(&:close)
      end

      private

      def read_header
        data = @file.read(12)
        @type = data[0, 4]
        @num_lumps = data[4, 4].unpack1('V')
        @directory_offset = data[8, 4].unpack1('V')
        @lump_cache = {}

        unless [@type == IWAD, @type == PWAD].any?
          raise Error, "Invalid WAD type: #{@type}"
        end
      end

      def read_directory
        @file.seek(@directory_offset)
        @directory = @num_lumps.times.map do
          data = @file.read(16)
          DirectoryEntry.new(
            data[0, 4].unpack1('V'),
            data[4, 4].unpack1('V'),
            data[8, 8].delete("\x00").upcase
          )
        end
      end
    end
  end
end

# frozen_string_literal: true

module Doom
  module Wad
    class Palette
      COLORS = 256
      PALETTES = 14
      PALETTE_SIZE = COLORS * 3

      attr_reader :colors

      def initialize(colors)
        @colors = colors
      end

      def [](index)
        @colors[index]
      end

      def self.load(wad, palette_index = 0)
        data = wad.read_lump('PLAYPAL')
        raise Error, 'PLAYPAL lump not found' unless data

        offset = palette_index * PALETTE_SIZE
        colors = COLORS.times.map do |i|
          [
            data[offset + i * 3].ord,
            data[offset + i * 3 + 1].ord,
            data[offset + i * 3 + 2].ord
          ]
        end

        new(colors)
      end
    end
  end
end

# frozen_string_literal: true

module Doom
  module Wad
    class Colormap
      MAPS = 34
      MAP_SIZE = 256

      attr_reader :maps

      def initialize(maps)
        @maps = maps
      end

      def [](map_index)
        @maps[map_index]
      end

      def self.load(wad)
        data = wad.read_lump('COLORMAP')
        raise Error, 'COLORMAP lump not found' unless data

        maps = MAPS.times.map do |i|
          offset = i * MAP_SIZE
          data[offset, MAP_SIZE].bytes
        end

        new(maps)
      end
    end
  end
end

# frozen_string_literal: true

module Doom
  module Wad
    class Flat
      WIDTH = 64
      HEIGHT = 64
      SIZE = WIDTH * HEIGHT

      attr_reader :name, :pixels

      def width
        WIDTH
      end

      def height
        HEIGHT
      end

      def initialize(name, pixels)
        @name = name
        @pixels = pixels
      end

      def [](x, y)
        @pixels[(y & 63) * WIDTH + (x & 63)]
      end

      def self.load_all(wad)
        entries = wad.lumps_between('F_START', 'F_END')
        entries.map do |entry|
          next if entry.size != SIZE

          data = wad.read_lump_at(entry)
          new(entry.name, data.bytes)
        end.compact
      end
    end
  end
end

# frozen_string_literal: true

module Doom
  module Wad
    class Patch
      Post = Struct.new(:top_delta, :pixels)

      attr_reader :name, :width, :height, :left_offset, :top_offset, :columns

      def initialize(name, width, height, left_offset, top_offset, columns)
        @name = name
        @width = width
        @height = height
        @left_offset = left_offset
        @top_offset = top_offset
        @columns = columns
      end

      def self.load(wad, name)
        data = wad.read_lump(name)
        return nil unless data

        parse(name, data)
      end

      def self.parse(name, data)
        width = data[0, 2].unpack1('v')
        height = data[2, 2].unpack1('v')
        left_offset = data[4, 2].unpack1('s<')
        top_offset = data[6, 2].unpack1('s<')

        column_offsets = width.times.map do |i|
          data[8 + i * 4, 4].unpack1('V')
        end

        columns = column_offsets.map do |offset|
          read_column(data, offset)
        end

        new(name, width, height, left_offset, top_offset, columns)
      end

      def self.read_column(data, offset)
        posts = []
        pos = offset

        loop do
          top_delta = data[pos].ord
          break if top_delta == 0xFF

          length = data[pos + 1].ord
          pixels = data[pos + 3, length].bytes
          posts << Post.new(top_delta, pixels)
          pos += length + 4
        end

        posts
      end
    end
  end
end

# frozen_string_literal: true

module Doom
  module Wad
    class Texture
      PatchRef = Struct.new(:x_offset, :y_offset, :patch_index)

      attr_reader :name, :width, :height, :patch_refs

      def initialize(name, width, height, patch_refs)
        @name = name
        @width = width
        @height = height
        @patch_refs = patch_refs
      end

      def self.load_all(wad)
        pnames = load_pnames(wad)
        textures = {}
        texture_names = []

        %w[TEXTURE1 TEXTURE2].each do |lump_name|
          data = wad.read_lump(lump_name)
          next unless data

          parse_texture_lump(data).each do |tex|
            textures[tex.name] = tex
            texture_names << tex.name
          end
        end

        { textures: textures, pnames: pnames, texture_names: texture_names }
      end

      def self.load_pnames(wad)
        data = wad.read_lump('PNAMES')
        return [] unless data

        count = data[0, 4].unpack1('V')
        count.times.map do |i|
          data[4 + i * 8, 8].delete("\x00").upcase
        end
      end

      def self.parse_texture_lump(data)
        num_textures = data[0, 4].unpack1('V')
        offsets = num_textures.times.map do |i|
          data[4 + i * 4, 4].unpack1('V')
        end

        offsets.map do |offset|
          parse_texture(data, offset)
        end
      end

      def self.parse_texture(data, offset)
        name = data[offset, 8].delete("\x00").upcase
        width = data[offset + 12, 2].unpack1('v')
        height = data[offset + 14, 2].unpack1('v')
        patch_count = data[offset + 20, 2].unpack1('v')

        patch_refs = patch_count.times.map do |i|
          po = offset + 22 + i * 10
          PatchRef.new(
            data[po, 2].unpack1('s<'),
            data[po + 2, 2].unpack1('s<'),
            data[po + 4, 2].unpack1('v')
          )
        end

        new(name, width, height, patch_refs)
      end
    end

    class TextureManager
      attr_reader :textures, :pnames, :patches, :texture_names

      def initialize(wad)
        @wad = wad
        result = Texture.load_all(wad)
        @textures = result[:textures]
        @pnames = result[:pnames]
        @texture_names = result[:texture_names]
        @patches = {}
        @composite_cache = {}
      end

      def [](name)
        return nil if name.nil? || name.empty? || name == '-'

        @composite_cache[name] ||= build_composite(name.upcase)
      end

      def get_patch(index)
        name = @pnames[index]
        return nil unless name

        @patches[name] ||= Patch.load(@wad, name)
      end

      private

      def build_composite(name)
        texture = @textures[name]
        return nil unless texture

        columns = Array.new(texture.width) { [] }

        texture.patch_refs.each do |pref|
          patch = get_patch(pref.patch_index)
          next unless patch

          patch.columns.each_with_index do |posts, px|
            tx = pref.x_offset + px
            next if tx < 0 || tx >= texture.width

            posts.each do |post|
              columns[tx] << Patch::Post.new(
                pref.y_offset + post.top_delta,
                post.pixels
              )
            end
          end
        end

        CompositeTexture.new(name, texture.width, texture.height, columns)
      end
    end

    class CompositeTexture
      attr_reader :name, :width, :height, :columns

      def initialize(name, width, height, columns)
        @name = name
        @width = width
        @height = height
        @columns = columns
        @column_cache = Array.new(width)
        precompute_columns
      end

      def column_pixels(x, _height_needed = nil)
        @column_cache[x & (@width - 1)]
      end

      private

      def precompute_columns
        @width.times do |x|
          posts = @columns[x]
          pixels = Array.new(@height)  # nil = transparent
          posts.each do |post|
            td = post.top_delta
            post.pixels.each_with_index do |color, i|
              y = (td + i) % @height
              pixels[y] = color
            end
          end
          @column_cache[x] = pixels.freeze
        end
      end
    end
  end
end

# frozen_string_literal: true

module Doom
  module Wad
    class Sprite
      attr_reader :name, :width, :height, :left_offset, :top_offset

      def initialize(name, width, height, left_offset, top_offset, columns)
        @name = name
        @width = width
        @height = height
        @left_offset = left_offset
        @top_offset = top_offset
        @columns = columns
      end

      def column_pixels(x)
        @columns[x] || []
      end

      # Load a sprite from a WAD patch lump
      def self.load(wad, lump_name)
        entry = wad.directory.find { |e| e.name == lump_name }
        return nil unless entry

        data = wad.read_lump_at(entry)
        return nil if data.size < 8

        width = data[0, 2].unpack1('v')
        height = data[2, 2].unpack1('v')
        left_offset = data[4, 2].unpack1('s<')
        top_offset = data[6, 2].unpack1('s<')

        # Read column offsets
        column_offsets = []
        width.times do |i|
          column_offsets << data[8 + i * 4, 4].unpack1('V')
        end

        # Read columns
        columns = []
        width.times do |x|
          column = Array.new(height, nil)  # nil = transparent
          offset = column_offsets[x]

          # Read posts for this column
          loop do
            break if offset >= data.size
            row_start = data[offset].ord
            break if row_start == 255  # End of column marker

            post_height = data[offset + 1].ord
            break if post_height == 0 || offset + 3 + post_height > data.size

            # Skip padding byte, read pixels, skip trailing padding
            post_height.times do |i|
              y = row_start + i
              if y < height
                column[y] = data[offset + 3 + i].ord
              end
            end

            offset += post_height + 4  # row_start + count + padding + pixels + padding
          end

          columns << column
        end

        new(lump_name, width, height, left_offset, top_offset, columns)
      end
    end

    class SpriteManager
      # Map thing types to sprite prefixes
      THING_SPRITES = {
        # Ammo
        2007 => 'CLIP', # Clip
        2048 => 'AMMO', # Box of ammo
        2008 => 'SHEL', # Shells
        2049 => 'SBOX', # Box of shells
        2010 => 'ROCK', # Rocket
        2046 => 'BROK', # Box of rockets
        2047 => 'CELL', # Cell charge
        17 => 'CELP',   # Cell pack

        # Weapons
        2001 => 'SHOT', # Shotgun
        2002 => 'MGUN', # Chaingun
        2003 => 'LAUN', # Rocket launcher
        2004 => 'PLAS', # Plasma rifle
        2006 => 'BFUG', # BFG 9000
        2005 => 'CSAW', # Chainsaw

        # Health/Armor
        2011 => 'STIM', # Stimpack
        2012 => 'MEDI', # Medikit
        2014 => 'BON1', # Health bonus
        2015 => 'BON2', # Armor bonus
        2018 => 'ARM1', # Green armor
        2019 => 'ARM2', # Blue armor

        # Keys
        5 => 'BKEY',    # Blue keycard
        6 => 'YKEY',    # Yellow keycard
        13 => 'RKEY',   # Red keycard
        40 => 'BSKU',   # Blue skull
        39 => 'YSKU',   # Yellow skull
        38 => 'RSKU',   # Red skull

        # Decorations
        2028 => 'COLU', # Light column
        30 => 'COL1',   # Tall green pillar
        31 => 'COL2',   # Short green pillar
        32 => 'COL3',   # Tall red pillar
        33 => 'COL4',   # Short red pillar
        34 => 'CAND',   # Candle
        44 => 'TBLU',   # Tall blue torch
        45 => 'TGRN',   # Tall green torch
        46 => 'TRED',   # Tall red torch
        48 => 'ELEC',   # Tall tech column
        35 => 'CBRA',   # Candelabra

        # Dead bodies / gore decorations
        10 => 'PLAY',   # Bloody mess
        12 => 'PLAY',   # Bloody mess 2
        15 => 'PLAY',   # Dead player
        24 => 'POL5',   # Pool of blood and flesh

        # Barrels
        2035 => 'BAR1', # Exploding barrel

        # Monsters
        3004 => 'POSS', # Zombieman
        9 => 'SPOS',    # Shotgun guy
        3001 => 'TROO', # Imp
        3002 => 'SARG', # Demon
        58 => 'SARG',   # Spectre (same as Demon)
        3003 => 'BOSS', # Baron of Hell
        3005 => 'HEAD', # Cacodemon
        3006 => 'SKUL', # Lost soul
        7 => 'SPID',    # Spider Mastermind
        16 => 'CYBR',   # Cyberdemon
      }.freeze

      # Things that use a specific frame instead of 'A'
      # From DOOM info.h mobjinfo spawnstate:
      # MT_MISC10 (type 10) -> S_PLAY_XDIE9 = PLAY W (gibbed mess)
      # MT_MISC12 (type 12) -> S_PLAY_DIE7 = PLAY N (dead body)
      # MT_MISC15 (type 15) -> S_PLAY_DIE7 = PLAY N (dead body)
      THING_DEFAULT_FRAME = {
        10 => 'W',   # Bloody mess (gibbed)
        12 => 'N',   # Bloody mess 2 (dead body flat)
        15 => 'N',   # Dead player (dead body flat)
      }.freeze

      def initialize(wad)
        @wad = wad
        @cache = {}
        @rotation_cache = {}

        # Build sprite lump index: maps "PREFIXframe_rotation" -> [lump_name, mirrored?]
        # Handles combined lumps like SPOSA2A8 (rotation 2 normal, rotation 8 mirrored)
        @sprite_index = {}
        build_sprite_index
      end

      # Get default sprite (rotation 0 or 1)
      def [](thing_type)
        return @cache[thing_type] if @cache.key?(thing_type)

        prefix = THING_SPRITES[thing_type]
        return nil unless prefix

        frame = THING_DEFAULT_FRAME[thing_type] || 'A'
        sprite = load_sprite_frame(prefix, frame, 0) ||
                 load_sprite_frame(prefix, frame, 1)

        @cache[thing_type] = sprite
        sprite
      end

      # Get sprite prefix for a thing type
      def prefix_for(thing_type)
        THING_SPRITES[thing_type]
      end

      # Get sprite for specific rotation (1-8, or 0 for all angles)
      # viewer_angle: angle from viewer to sprite in radians
      # thing_angle: thing's facing angle in degrees
      def get_rotated(thing_type, viewer_angle, thing_angle)
        prefix = THING_SPRITES[thing_type]
        return nil unless prefix

        frame = THING_DEFAULT_FRAME[thing_type] || 'A'

        # Check for rotation 0 (all angles) sprite first
        sprite = load_sprite_frame(prefix, frame, 0)
        return sprite if sprite

        # Calculate rotation frame (1-8)
        # DOOM: rot = (R_PointToAngle(thing) - thing->angle + ANG45/2*9) >> 29
        # Rotation 1=front (viewer faces monster's front), 5=back
        angle_diff = viewer_angle - (thing_angle * Math::PI / 180.0) + Math::PI
        angle_diff = angle_diff % (2 * Math::PI)
        angle_diff += 2 * Math::PI if angle_diff < 0
        rotation = ((angle_diff + Math::PI / 8) / (Math::PI / 4)).to_i % 8 + 1

        load_sprite_frame(prefix, frame, rotation) || @cache[thing_type]
      end

      # Get a specific frame (for death animations, etc.)
      def get_frame(thing_type, frame_letter, viewer_angle, thing_angle)
        prefix = THING_SPRITES[thing_type]
        return nil unless prefix

        # Death frames typically use rotation 0 (same from all angles)
        sprite = load_sprite_frame(prefix, frame_letter, 0)
        return sprite if sprite

        # Try with calculated rotation (same formula as get_rotated)
        angle_diff = viewer_angle - (thing_angle * Math::PI / 180.0) + Math::PI
        angle_diff = angle_diff % (2 * Math::PI)
        angle_diff += 2 * Math::PI if angle_diff < 0
        rotation = ((angle_diff + Math::PI / 8) / (Math::PI / 4)).to_i % 8 + 1

        load_sprite_frame(prefix, frame_letter, rotation)
      end

      # Get a frame by explicit prefix (for barrel explosions where prefix differs from thing type)
      def get_frame_by_prefix(prefix, frame_letter)
        load_sprite_frame(prefix, frame_letter, 0)
      end

      private

      def build_sprite_index
        @wad.directory.each do |entry|
          name = entry.name
          next if name.length < 6

          prefix = name[0, 4]
          frame1 = name[4]
          rot1 = name[5].to_i

          # Register first frame+rotation
          key = "#{prefix}#{frame1}#{rot1}"
          @sprite_index[key] = [name, false]

          # Check for mirrored second rotation (e.g., SPOSA2A8)
          if name.length >= 8
            frame2 = name[6]
            rot2 = name[7].to_i
            key2 = "#{prefix}#{frame2}#{rot2}"
            @sprite_index[key2] = [name, true]
          end
        end
      end

      def load_sprite_frame(prefix, frame, rotation)
        key = "#{prefix}#{frame}#{rotation}"
        return @rotation_cache[key] if @rotation_cache.key?(key)

        index_entry = @sprite_index[key]
        unless index_entry
          @rotation_cache[key] = nil
          return nil
        end

        lump_name, mirrored = index_entry
        # Load the base sprite (may be shared by mirrored pair)
        base = load_or_cache_lump(lump_name)
        unless base
          @rotation_cache[key] = nil
          return nil
        end

        sprite = mirrored ? mirror_sprite(base) : base
        @rotation_cache[key] = sprite
        sprite
      end

      def load_or_cache_lump(lump_name)
        cache_key = "_lump_#{lump_name}"
        return @rotation_cache[cache_key] if @rotation_cache.key?(cache_key)

        sprite = Sprite.load(@wad, lump_name)
        @rotation_cache[cache_key] = sprite
        sprite
      end

      def mirror_sprite(sprite)
        # Flip columns horizontally, adjust left_offset
        mirrored_columns = sprite.instance_variable_get(:@columns).reverse
        mirrored_left = sprite.width - sprite.left_offset
        Sprite.new(
          "#{sprite.name}_M",
          sprite.width, sprite.height,
          mirrored_left, sprite.top_offset,
          mirrored_columns
        )
      end
    end
  end
end

# frozen_string_literal: true

module Doom
  module Wad
    # Loads HUD graphics (status bar, weapons) from WAD
    class HudGraphics
      attr_reader :status_bar, :arms_background, :numbers, :grey_numbers, :yellow_numbers, :weapons, :faces, :keys

      def initialize(wad)
        @wad = wad
        @cache = {}

        load_status_bar
        load_numbers
        load_weapons
        load_faces
        load_keys
      end

      # Get a cached graphic by name
      def [](name)
        @cache[name]
      end

      private

      def load_graphic(name)
        return @cache[name] if @cache[name]

        entry = @wad.find_lump(name)
        return nil unless entry

        data = @wad.read_lump_at(entry)
        return nil unless data && data.size > 8

        sprite = parse_patch(name, data)
        @cache[name] = sprite
        sprite
      end

      def parse_patch(name, data)
        width = data[0, 2].unpack1('v')
        height = data[2, 2].unpack1('v')
        left_offset = data[4, 2].unpack1('s<')
        top_offset = data[6, 2].unpack1('s<')

        # Read column offsets
        column_offsets = width.times.map do |i|
          data[8 + i * 4, 4].unpack1('V')
        end

        # Build column data
        columns = column_offsets.map do |offset|
          read_column(data, offset, height)
        end

        HudSprite.new(name, width, height, left_offset, top_offset, columns)
      end

      def read_column(data, offset, height)
        pixels = Array.new(height)
        pos = offset

        loop do
          break if pos >= data.size
          top_delta = data[pos].ord
          break if top_delta == 0xFF

          length = data[pos + 1].ord
          # Skip padding byte, read pixels, skip end padding
          pixel_data = data[pos + 3, length]
          break unless pixel_data

          pixel_data.bytes.each_with_index do |color, i|
            y = top_delta + i
            pixels[y] = color if y < height
          end

          pos += length + 4
        end

        pixels
      end

      def load_status_bar
        @status_bar = load_graphic('STBAR')
        @arms_background = load_graphic('STARMS')
      end

      def load_numbers
        @numbers = {}
        # Large red numbers for health/ammo
        (0..9).each do |n|
          @numbers[n] = load_graphic("STTNUM#{n}")
        end
        @numbers['-'] = load_graphic('STTMINUS')
        @numbers['%'] = load_graphic('STTPRCNT')

        # Small grey numbers for arms (weapon not owned)
        @grey_numbers = {}
        (0..9).each do |n|
          @grey_numbers[n] = load_graphic("STGNUM#{n}")
        end

        # Small yellow numbers for arms (weapon owned) and ammo counts
        @yellow_numbers = {}
        (0..9).each do |n|
          @yellow_numbers[n] = load_graphic("STYSNUM#{n}")
        end
      end

      def load_weapons
        @weapons = {}

        # Pistol frames (PISG = pistol gun)
        @weapons[:pistol] = {
          idle: load_graphic('PISGA0'),
          fire: [
            load_graphic('PISGB0'),
            load_graphic('PISGC0'),
            load_graphic('PISGD0'),
            load_graphic('PISGE0')
          ].compact,
          flash: [
            load_graphic('PISFA0'),
            load_graphic('PISFB0')
          ].compact
        }

        # Fist frames (PUNG = punch)
        @weapons[:fist] = {
          idle: load_graphic('PUNGA0'),
          fire: [
            load_graphic('PUNGB0'),
            load_graphic('PUNGC0'),
            load_graphic('PUNGD0')
          ].compact
        }

        # Shotgun (SHTG)
        @weapons[:shotgun] = {
          idle: load_graphic('SHTGA0'),
          fire: [
            load_graphic('SHTGB0'),
            load_graphic('SHTGC0'),
            load_graphic('SHTGD0')
          ].compact,
          flash: [load_graphic('SHTFA0'), load_graphic('SHTFB0')].compact
        }

        # Chaingun (CHGG)
        @weapons[:chaingun] = {
          idle: load_graphic('CHGGA0'),
          fire: [
            load_graphic('CHGGB0'),
            load_graphic('CHGGC0')
          ].compact,
          flash: [load_graphic('CHGFA0'), load_graphic('CHGFB0')].compact
        }

        # Rocket launcher (MISG)
        @weapons[:rocket] = {
          idle: load_graphic('MISGA0'),
          fire: [
            load_graphic('MISGB0'),
            load_graphic('MISGC0'),
            load_graphic('MISGD0')
          ].compact,
          flash: [load_graphic('MISFA0'), load_graphic('MISFB0'), load_graphic('MISFC0')].compact
        }

        # Plasma rifle (PLSG)
        @weapons[:plasma] = {
          idle: load_graphic('PLSGA0'),
          fire: [
            load_graphic('PLSGB0')
          ].compact,
          flash: [load_graphic('PLSFA0'), load_graphic('PLSFB0')].compact
        }

        # BFG9000 (BFGG)
        @weapons[:bfg] = {
          idle: load_graphic('BFGGA0'),
          fire: [
            load_graphic('BFGGB0'),
            load_graphic('BFGGC0')
          ].compact,
          flash: [load_graphic('BFGFA0'), load_graphic('BFGFB0')].compact
        }

        # Chainsaw (SAWG)
        @weapons[:chainsaw] = {
          idle: load_graphic('SAWGA0'),
          fire: [
            load_graphic('SAWGB0'),
            load_graphic('SAWGC0'),
            load_graphic('SAWGD0')
          ].compact
        }
      end

      def load_faces
        @faces = {}

        # Straight ahead faces at different health levels
        # STF = status face, ST = straight, 0-4 = health level (4=full, 0=dying)
        (0..4).each do |health_level|
          @faces[health_level] = {
            straight: [
              load_graphic("STFST#{health_level}0"),
              load_graphic("STFST#{health_level}1"),
              load_graphic("STFST#{health_level}2")
            ].compact,
            left: load_graphic("STFTL#{health_level}0"),
            right: load_graphic("STFTR#{health_level}0"),
            ouch: load_graphic("STFOUCH#{health_level}"),
            evil: load_graphic("STFEVL#{health_level}"),
            kill: load_graphic("STFKILL#{health_level}")
          }
        end

        # Special faces
        @faces[:dead] = load_graphic('STFDEAD0')
        @faces[:god] = load_graphic('STFGOD0')
      end

      def load_keys
        @keys = {
          blue_card: load_graphic('STKEYS0'),
          yellow_card: load_graphic('STKEYS1'),
          red_card: load_graphic('STKEYS2'),
          blue_skull: load_graphic('STKEYS3'),
          yellow_skull: load_graphic('STKEYS4'),
          red_skull: load_graphic('STKEYS5')
        }
      end
    end

    # Simple sprite container for HUD graphics
    class HudSprite
      attr_reader :name, :width, :height, :left_offset, :top_offset, :columns

      def initialize(name, width, height, left_offset, top_offset, columns)
        @name = name
        @width = width
        @height = height
        @left_offset = left_offset
        @top_offset = top_offset
        @columns = columns
      end

      def column_pixels(x)
        @columns[x % @width]
      end
    end
  end
end

# frozen_string_literal: true

module Doom
  module Map
    Vertex = Struct.new(:x, :y)

    Thing = Struct.new(:x, :y, :angle, :type, :flags)

    Linedef = Struct.new(:v1, :v2, :flags, :special, :tag, :sidedef_right, :sidedef_left) do
      FLAGS = {
        BLOCKING: 0x0001,
        BLOCKMONSTERS: 0x0002,
        TWOSIDED: 0x0004,
        DONTPEGTOP: 0x0008,
        DONTPEGBOTTOM: 0x0010,
        SECRET: 0x0020,
        SOUNDBLOCK: 0x0040,
        DONTDRAW: 0x0080,
        MAPPED: 0x0100
      }.freeze

      def two_sided?
        (flags & FLAGS[:TWOSIDED]) != 0
      end

      def upper_unpegged?
        (flags & FLAGS[:DONTPEGTOP]) != 0
      end

      def lower_unpegged?
        (flags & FLAGS[:DONTPEGBOTTOM]) != 0
      end
    end

    Sidedef = Struct.new(:x_offset, :y_offset, :upper_texture, :lower_texture, :middle_texture, :sector)

    Sector = Struct.new(:floor_height, :ceiling_height, :floor_texture, :ceiling_texture, :light_level, :special, :tag)

    Seg = Struct.new(:v1, :v2, :angle, :linedef, :direction, :offset)

    Subsector = Struct.new(:seg_count, :first_seg)

    class Node
      SUBSECTOR_FLAG = 0x8000

      attr_reader :x, :y, :dx, :dy, :bbox_right, :bbox_left, :child_right, :child_left

      BBox = Struct.new(:top, :bottom, :left, :right)

      def initialize(x, y, dx, dy, bbox_right, bbox_left, child_right, child_left)
        @x = x
        @y = y
        @dx = dx
        @dy = dy
        @bbox_right = bbox_right
        @bbox_left = bbox_left
        @child_right = child_right
        @child_left = child_left
      end

      def right_is_subsector?
        (@child_right & SUBSECTOR_FLAG) != 0
      end

      def left_is_subsector?
        (@child_left & SUBSECTOR_FLAG) != 0
      end

      def right_index
        @child_right & ~SUBSECTOR_FLAG
      end

      def left_index
        @child_left & ~SUBSECTOR_FLAG
      end
    end

    class MapData
      attr_reader :name, :things, :vertices, :linedefs, :sidedefs, :sectors, :segs, :subsectors, :nodes

      def initialize(name)
        @name = name
        @things = []
        @vertices = []
        @linedefs = []
        @sidedefs = []
        @sectors = []
        @segs = []
        @subsectors = []
        @nodes = []
      end

      def self.load(wad, map_name)
        map = new(map_name)

        lump_idx = wad.directory.index { |e| e.name == map_name.upcase }
        raise Error, "Map #{map_name} not found" unless lump_idx

        map.load_things(wad.read_lump_at(wad.directory[lump_idx + 1]))
        map.load_linedefs(wad.read_lump_at(wad.directory[lump_idx + 2]))
        map.load_sidedefs(wad.read_lump_at(wad.directory[lump_idx + 3]))
        map.load_vertices(wad.read_lump_at(wad.directory[lump_idx + 4]))
        map.load_segs(wad.read_lump_at(wad.directory[lump_idx + 5]))
        map.load_subsectors(wad.read_lump_at(wad.directory[lump_idx + 6]))
        map.load_nodes(wad.read_lump_at(wad.directory[lump_idx + 7]))
        map.load_sectors(wad.read_lump_at(wad.directory[lump_idx + 8]))

        # BLOCKMAP is at lump +10 (REJECT is +9). Optional -- parse defensively.
        blockmap_entry = wad.directory[lump_idx + 10]
        if blockmap_entry && blockmap_entry.name == 'BLOCKMAP'
          map.load_blockmap(wad.read_lump_at(blockmap_entry))
        end

        map
      end

      def load_things(data)
        count = data.size / 10
        count.times do |i|
          offset = i * 10
          @things << Thing.new(
            data[offset, 2].unpack1('s<'),
            data[offset + 2, 2].unpack1('s<'),
            data[offset + 4, 2].unpack1('v'),
            data[offset + 6, 2].unpack1('v'),
            data[offset + 8, 2].unpack1('v')
          )
        end
      end

      def load_vertices(data)
        count = data.size / 4
        count.times do |i|
          offset = i * 4
          @vertices << Vertex.new(
            data[offset, 2].unpack1('s<'),
            data[offset + 2, 2].unpack1('s<')
          )
        end
      end

      def load_linedefs(data)
        count = data.size / 14
        count.times do |i|
          offset = i * 14
          @linedefs << Linedef.new(
            data[offset, 2].unpack1('v'),
            data[offset + 2, 2].unpack1('v'),
            data[offset + 4, 2].unpack1('v'),
            data[offset + 6, 2].unpack1('v'),
            data[offset + 8, 2].unpack1('v'),
            data[offset + 10, 2].unpack1('s<'),
            data[offset + 12, 2].unpack1('s<')
          )
        end
      end

      def load_sidedefs(data)
        count = data.size / 30
        count.times do |i|
          offset = i * 30
          @sidedefs << Sidedef.new(
            data[offset, 2].unpack1('s<'),
            data[offset + 2, 2].unpack1('s<'),
            data[offset + 4, 8].delete("\x00").strip,
            data[offset + 12, 8].delete("\x00").strip,
            data[offset + 20, 8].delete("\x00").strip,
            data[offset + 28, 2].unpack1('v')
          )
        end
      end

      def load_sectors(data)
        count = data.size / 26
        count.times do |i|
          offset = i * 26
          @sectors << Sector.new(
            data[offset, 2].unpack1('s<'),
            data[offset + 2, 2].unpack1('s<'),
            data[offset + 4, 8].delete("\x00").strip,
            data[offset + 12, 8].delete("\x00").strip,
            data[offset + 20, 2].unpack1('v'),
            data[offset + 22, 2].unpack1('v'),
            data[offset + 24, 2].unpack1('v')
          )
        end
      end

      def load_segs(data)
        count = data.size / 12
        count.times do |i|
          offset = i * 12
          @segs << Seg.new(
            data[offset, 2].unpack1('v'),
            data[offset + 2, 2].unpack1('v'),
            data[offset + 4, 2].unpack1('s<'),
            data[offset + 6, 2].unpack1('v'),
            data[offset + 8, 2].unpack1('v'),
            data[offset + 10, 2].unpack1('s<')
          )
        end
      end

      def load_subsectors(data)
        count = data.size / 4
        count.times do |i|
          offset = i * 4
          @subsectors << Subsector.new(
            data[offset, 2].unpack1('v'),
            data[offset + 2, 2].unpack1('v')
          )
        end
      end

      def load_nodes(data)
        count = data.size / 28
        count.times do |i|
          offset = i * 28
          bbox_right = Node::BBox.new(
            data[offset + 8, 2].unpack1('s<'),
            data[offset + 10, 2].unpack1('s<'),
            data[offset + 12, 2].unpack1('s<'),
            data[offset + 14, 2].unpack1('s<')
          )
          bbox_left = Node::BBox.new(
            data[offset + 16, 2].unpack1('s<'),
            data[offset + 18, 2].unpack1('s<'),
            data[offset + 20, 2].unpack1('s<'),
            data[offset + 22, 2].unpack1('s<')
          )
          @nodes << Node.new(
            data[offset, 2].unpack1('s<'),
            data[offset + 2, 2].unpack1('s<'),
            data[offset + 4, 2].unpack1('s<'),
            data[offset + 6, 2].unpack1('s<'),
            bbox_right,
            bbox_left,
            data[offset + 24, 2].unpack1('v'),
            data[offset + 26, 2].unpack1('v')
          )
        end
      end

      def player_start
        @things.find { |t| t.type == 1 }
      end

      # BLOCKMAP: 128-unit grid index into linedefs. Each block lists the
      # linedefs that touch it. Used for fast collision lookup.
      BLOCKMAP_BLOCK_SIZE = 128

      def load_blockmap(data)
        return if data.nil? || data.size < 8

        @blockmap_origin_x = data[0, 2].unpack1('s<')
        @blockmap_origin_y = data[2, 2].unpack1('s<')
        @blockmap_cols = data[4, 2].unpack1('s<')
        @blockmap_rows = data[6, 2].unpack1('s<')
        return if @blockmap_cols <= 0 || @blockmap_rows <= 0

        block_count = @blockmap_cols * @blockmap_rows
        @blockmap_blocks = Array.new(block_count)

        block_count.times do |i|
          offset_words = data[8 + i * 2, 2].unpack1('v')
          byte_offset = offset_words * 2
          linedefs_in_block = []
          ptr = byte_offset
          # Skip the leading 0x0000 sentinel that some blockmaps include.
          ptr += 2 if ptr + 2 <= data.size && data[ptr, 2].unpack1('s<') == 0
          while ptr + 2 <= data.size
            idx = data[ptr, 2].unpack1('s<')
            break if idx == -1  # 0xFFFF terminator
            linedefs_in_block << idx
            ptr += 2
          end
          @blockmap_blocks[i] = linedefs_in_block
        end
      end

      # Yield each linedef whose block overlaps the bounding box (min_x, min_y,
      # max_x, max_y). Yields each linedef at most once per call. Falls back
      # to iterating all linedefs if no blockmap is loaded.
      def each_linedef_near(min_x, min_y, max_x, max_y)
        unless @blockmap_blocks
          @linedefs.each { |ld| yield ld }
          return
        end

        bx0 = ((min_x - @blockmap_origin_x) / BLOCKMAP_BLOCK_SIZE).floor.clamp(0, @blockmap_cols - 1)
        bx1 = ((max_x - @blockmap_origin_x) / BLOCKMAP_BLOCK_SIZE).floor.clamp(0, @blockmap_cols - 1)
        by0 = ((min_y - @blockmap_origin_y) / BLOCKMAP_BLOCK_SIZE).floor.clamp(0, @blockmap_rows - 1)
        by1 = ((max_y - @blockmap_origin_y) / BLOCKMAP_BLOCK_SIZE).floor.clamp(0, @blockmap_rows - 1)

        seen = {}
        by0.upto(by1) do |by|
          row_base = by * @blockmap_cols
          bx0.upto(bx1) do |bx|
            indices = @blockmap_blocks[row_base + bx]
            next unless indices
            indices.each do |idx|
              next if seen[idx]
              seen[idx] = true
              yield @linedefs[idx]
            end
          end
        end
      end

      def blockmap_loaded?
        !@blockmap_blocks.nil?
      end

      # Find the sector at a given position by traversing the BSP tree
      def sector_at(x, y)
        subsector = subsector_at(x, y)
        return nil unless subsector

        # Get sector from first seg of subsector
        seg = @segs[subsector.first_seg]
        return nil unless seg

        linedef = @linedefs[seg.linedef]
        sidedef_idx = seg.direction == 0 ? linedef.sidedef_right : linedef.sidedef_left
        return nil if sidedef_idx < 0

        @sectors[@sidedefs[sidedef_idx].sector]
      end

      # Find the subsector containing a point
      def subsector_at(x, y)
        node_idx = @nodes.size - 1
        while (node_idx & Node::SUBSECTOR_FLAG) == 0
          node = @nodes[node_idx]
          side = point_on_side(x, y, node)
          node_idx = side == 0 ? node.child_right : node.child_left
        end
        @subsectors[node_idx & ~Node::SUBSECTOR_FLAG]
      end

      private

      def point_on_side(x, y, node)
        dx = x - node.x
        dy = y - node.y
        left = dy * node.dx
        right = dx * node.dy
        right >= left ? 0 : 1
      end
    end
  end
end

# frozen_string_literal: true

module Doom
  module Game
    # Tracks player state for HUD display and weapon rendering
    class PlayerState
      # Weapons
      WEAPON_FIST = 0
      WEAPON_PISTOL = 1
      WEAPON_SHOTGUN = 2
      WEAPON_CHAINGUN = 3
      WEAPON_ROCKET = 4
      WEAPON_PLASMA = 5
      WEAPON_BFG = 6
      WEAPON_CHAINSAW = 7

      # Weapon symbols for graphics lookup
      WEAPON_NAMES = {
        WEAPON_FIST => :fist,
        WEAPON_PISTOL => :pistol,
        WEAPON_SHOTGUN => :shotgun,
        WEAPON_CHAINGUN => :chaingun,
        WEAPON_ROCKET => :rocket,
        WEAPON_PLASMA => :plasma,
        WEAPON_BFG => :bfg,
        WEAPON_CHAINSAW => :chainsaw
      }.freeze

      # Attack durations in tics (at 35fps), matching DOOM's weapon state sequences
      ATTACK_DURATIONS = {
        WEAPON_FIST => 14,       # Punch windup + swing
        WEAPON_PISTOL => 16,     # S_PISTOL: 6+4+5+1 tics
        WEAPON_SHOTGUN => 40,    # Pump action cycle
        WEAPON_CHAINGUN => 8,    # Rapid fire (2 shots per cycle)
        WEAPON_ROCKET => 20,     # Rocket launch + recovery
        WEAPON_PLASMA => 8,      # Fast energy weapon
        WEAPON_BFG => 60,        # Long charge + fire
        WEAPON_CHAINSAW => 6     # Fast melee
      }.freeze

      attr_accessor :health, :armor, :max_health, :max_armor
      attr_accessor :ammo_bullets, :ammo_shells, :ammo_rockets, :ammo_cells
      attr_accessor :max_bullets, :max_shells, :max_rockets, :max_cells
      attr_accessor :weapon, :has_weapons
      attr_accessor :keys
      attr_accessor :attacking, :attack_frame, :attack_tics
      attr_accessor :bob_angle, :bob_amount
      attr_accessor :is_moving
      attr_accessor :dead, :death_tic
      attr_accessor :damage_count  # Red flash intensity (0-8), decays each tic
      attr_accessor :god_mode, :infinite_ammo

      # Smooth step-up/down (matching Chocolate Doom's P_CalcHeight / P_ZMovement)
      VIEWHEIGHT = 41.0
      VIEWHEIGHT_HALF = VIEWHEIGHT / 2.0
      DELTA_ACCEL = 0.25        # deltaviewheight += FRACUNIT/4 per tic
      attr_reader :viewheight, :deltaviewheight

      # View bob (camera bounce when walking, matching Chocolate Doom's
      # P_CalcHeight + P_XYMovement + P_Thrust from p_user.c / p_mobj.c)
      MAXBOB = 16.0           # Maximum bob amplitude (0x100000 in fixed-point = 16 map units)
      STOPSPEED = 0.0625      # Snap-to-zero threshold (0x1000 in fixed-point)
      # Continuous-time equivalents of DOOM's per-tic constants (35 fps tic rate):
      #   FRICTION = 0xE800/0x10000 = 0.90625 per tic
      #   decay_rate = -ln(0.90625) * 35 = 3.44/sec
      #   walk thrust = forwardmove(25) * 2048 / 65536 = 0.78 map units/tic = 27.3/sec
      #   terminal velocity = 27.3 / 3.44 = 7.56 -> bob = 7.56^2/4 = 14.3 (89% of MAXBOB)
      BOB_DECAY_RATE = 3.44   # Friction as continuous decay rate (1/sec)
      BOB_THRUST = 26.0       # Walk thrust (map units/sec), gives terminal ~7.5
      BOB_FREQUENCY = 11.0    # Bob cycle frequency (rad/sec): FINEANGLES/20 * 35 / 8192 * 2*PI
      attr_reader :view_bob_offset

      def initialize
        reset
      end

      def reset
        @health = 100
        @armor = 0
        @max_health = 100
        @max_armor = 200

        # Ammo
        @ammo_bullets = 50
        @ammo_shells = 0
        @ammo_rockets = 0
        @ammo_cells = 0

        @max_bullets = 200
        @max_shells = 50
        @max_rockets = 50
        @max_cells = 300

        # Start with fist and pistol
        @weapon = WEAPON_PISTOL
        @has_weapons = [true, true, false, false, false, false, false, false]

        # No keys
        @keys = {
          blue_card: false,
          yellow_card: false,
          red_card: false,
          blue_skull: false,
          yellow_skull: false,
          red_skull: false
        }

        # Attack state
        @attacking = false
        @attack_frame = 0
        @attack_tics = 0

        # Death state
        @dead = false
        @death_tic = 0
        @damage_count = 0

        # Cheats
        @god_mode = false
        @infinite_ammo = false

        # Weapon bob
        @bob_angle = 0.0
        @bob_amount = 0.0
        @is_moving = false

        # Smooth step height (P_CalcHeight viewheight/deltaviewheight)
        @viewheight = VIEWHEIGHT
        @deltaviewheight = 0.0

        # View bob (camera bounce) - simulated momentum for P_CalcHeight
        @view_bob_offset = 0.0
        @momx = 0.0        # Simulated X momentum (map units/sec, not actual movement)
        @momy = 0.0        # Simulated Y momentum
        @thrust_x = 0.0    # Per-frame thrust input (raw, before normalization)
        @thrust_y = 0.0
        @view_bob_angle = 0.0
      end

      def weapon_name
        WEAPON_NAMES[@weapon]
      end

      def current_ammo
        case @weapon
        when WEAPON_PISTOL, WEAPON_CHAINGUN
          @ammo_bullets
        when WEAPON_SHOTGUN
          @ammo_shells
        when WEAPON_ROCKET
          @ammo_rockets
        when WEAPON_PLASMA, WEAPON_BFG
          @ammo_cells
        else
          nil # Fist/chainsaw don't use ammo
        end
      end

      def max_ammo_for_weapon
        case @weapon
        when WEAPON_PISTOL, WEAPON_CHAINGUN
          @max_bullets
        when WEAPON_SHOTGUN
          @max_shells
        when WEAPON_ROCKET
          @max_rockets
        when WEAPON_PLASMA, WEAPON_BFG
          @max_cells
        else
          nil
        end
      end

      def can_attack?
        return true if @weapon == WEAPON_FIST || @weapon == WEAPON_CHAINSAW
        return true if @infinite_ammo

        ammo = current_ammo
        ammo && ammo > 0
      end

      def start_attack
        return unless can_attack?
        return if @attacking

        @attacking = true
        @attack_frame = 0
        @attack_tics = 0

        # Consume ammo (skipped with infinite ammo)
        return if @infinite_ammo

        case @weapon
        when WEAPON_PISTOL
          @ammo_bullets -= 1 if @ammo_bullets > 0
        when WEAPON_SHOTGUN
          @ammo_shells -= 1 if @ammo_shells > 0
        when WEAPON_CHAINGUN
          @ammo_bullets -= 1 if @ammo_bullets > 0
        when WEAPON_ROCKET
          @ammo_rockets -= 1 if @ammo_rockets > 0
        when WEAPON_PLASMA
          @ammo_cells -= 1 if @ammo_cells > 0
        when WEAPON_BFG
          @ammo_cells -= 40 if @ammo_cells >= 40
        end
      end

      def update_attack
        return unless @attacking

        @attack_tics += 1

        # Calculate which frame we're on based on tics
        duration = ATTACK_DURATIONS[@weapon] || 8
        frame_count = @weapon == WEAPON_FIST ? 3 : 4

        tics_per_frame = duration / frame_count
        @attack_frame = (@attack_tics / tics_per_frame).to_i

        # Attack finished?
        if @attack_tics >= duration
          @attacking = false
          @attack_frame = 0
          @attack_tics = 0
        end
      end

      def update_bob(delta_time)
        if @is_moving
          # Increase bob while moving
          @bob_angle += delta_time * 10.0
          @bob_amount = [@bob_amount + delta_time * 16.0, 6.0].min
        else
          # Decay bob when stopped
          @bob_amount = [@bob_amount - delta_time * 12.0, 0.0].max
        end
      end

      # Called when player moves onto a different floor height.
      # Matches Chocolate Doom P_ZMovement: reduce viewheight by the step amount
      # so the camera doesn't snap, then let P_CalcHeight recover it smoothly.
      def notify_step(step_amount)
        return if step_amount == 0
        @viewheight -= step_amount
        @deltaviewheight = (VIEWHEIGHT - @viewheight) / 8.0
      end

      # Called on landing after a fall. Matches Chocolate Doom P_ZMovement:
      # deltaviewheight = momz >> 3, producing a squat that update_viewheight
      # then recovers via DELTA_ACCEL.
      def apply_fall_impact(momz)
        @deltaviewheight = momz / 8.0
      end

      # Gradually restore viewheight to VIEWHEIGHT (called each tic).
      # Matches Chocolate Doom P_CalcHeight viewheight recovery loop.
      # For step-up: viewheight < 41, delta > 0, accelerates upward.
      # For step-down: viewheight > 41, delta < 0, decelerates then recovers.
      def update_viewheight
        @viewheight += @deltaviewheight

        if @viewheight > VIEWHEIGHT && @deltaviewheight >= 0
          @viewheight = VIEWHEIGHT
          @deltaviewheight = 0.0
        end

        if @viewheight < VIEWHEIGHT_HALF
          @viewheight = VIEWHEIGHT_HALF
          @deltaviewheight = 1.0 if @deltaviewheight <= 0
        end

        if @deltaviewheight != 0
          @deltaviewheight += DELTA_ACCEL
          @deltaviewheight = 0.0 if @deltaviewheight.abs < 0.01 && (@viewheight - VIEWHEIGHT).abs < 0.5
        end
      end

      # Set movement momentum directly (called from GosuWindow with actual
      # movement momentum, which already has thrust + friction applied).
      def set_movement_momentum(momx, momy)
        @momx = momx
        @momy = momy
      end

      # Compute view bob from actual movement momentum.
      # Matches Chocolate Doom P_CalcHeight:
      #   bob = (momx*momx + momy*momy) >> 2, capped at MAXBOB
      #   viewz += finesine[angle] * bob/2
      # Momentum is in units/sec; DOOM's bob uses units/tic (divide by 35).
      BOB_MOM_SCALE = 1.0 / (35.0 * 35.0 * 4.0)  # (mom/35)^2 / 4

      def update_view_bob(delta_time)
        dt = delta_time.clamp(0.001, 0.05)

        # P_CalcHeight: bob = (momx_per_tic^2 + momy_per_tic^2) / 4, capped at MAXBOB
        bob = (@momx * @momx + @momy * @momy) * BOB_MOM_SCALE
        bob = MAXBOB if bob > MAXBOB

        # Advance bob sine wave (FINEANGLES/20 per tic = ~11 rad/sec)
        @view_bob_angle += BOB_FREQUENCY * dt

        # viewz offset: sin(angle) * bob/2
        @view_bob_offset = Math.sin(@view_bob_angle) * bob / 2.0
      end

      def weapon_bob_x
        Math.cos(@bob_angle) * @bob_amount
      end

      def weapon_bob_y
        Math.sin(@bob_angle * 2) * @bob_amount * 0.5
      end

      def health_level
        # 0 = dying, 4 = full health
        case @health
        when 80..200 then 4
        when 60..79 then 3
        when 40..59 then 2
        when 20..39 then 1
        else 0
        end
      end

      def switch_weapon(weapon_num)
        return unless weapon_num >= 0 && weapon_num < 8
        return unless @has_weapons[weapon_num]
        return if @attacking

        @weapon = weapon_num
      end

      # Apply damage (from environment or enemies). Armor absorbs some.
      def take_damage(amount)
        return if @dead
        return if @god_mode

        absorbed = 0
        if @armor > 0
          absorbed = amount / 3  # Green armor absorbs 1/3
          absorbed = @armor if absorbed > @armor
          @armor -= absorbed
        end

        actual = amount - absorbed
        @health -= actual

        # Red flash proportional to damage (capped at palette 8)
        @damage_count = [(@damage_count + actual / 2.0).ceil, 8].min

        if @health <= 0
          @health = 0
          @damage_count = 8
          die
        end
      end

      # Decay damage flash each tic
      def update_damage_count
        @damage_count -= 1 if @damage_count > 0
      end

      def die
        @dead = true
        @death_tic = 0
        @attacking = false
        @deltaviewheight = -VIEWHEIGHT / 8.0  # View drops to ground
      end
    end
  end
end

# frozen_string_literal: true


module Doom
  module Game
    # Manages animated sector actions (doors, lifts, etc.)
    class SectorActions
      # Door states
      DOOR_CLOSED = 0
      DOOR_OPENING = 1
      DOOR_OPEN = 2
      DOOR_CLOSING = 3

      # Door speeds (units per tic, 35 tics/sec)
      DOOR_SPEED = 2
      DOOR_WAIT = 150  # Tics to wait when open (~4 seconds)
      PLAYER_HEIGHT = 56

      # Lift constants
      LIFT_SPEED = 4
      LIFT_WAIT = 105  # ~3 seconds

      attr_reader :exit_triggered, :secrets_found

      def pop_teleport
        dest = @teleport_dest
        @teleport_dest = nil
        dest
      end

      def initialize(map, sound_engine = nil)
        @map = map
        @sound = sound_engine
        @active_doors = {}   # sector_index => door_state
        @active_lifts = {}   # sector_index => lift_state
        @player_x = 0
        @player_y = 0
        @exit_triggered = nil
        @secrets_found = {}  # sector_index => true
        @crossed_linedefs = {}
      end

      def update_player_position(x, y)
        @player_x = x
        @player_y = y
      end

      def update
        update_doors
        update_lifts
        check_walk_triggers
        check_secrets
      end

      # Try to use a linedef (called when player presses use key)
      def use_linedef(linedef, linedef_idx)
        return false if linedef.special == 0

        case linedef.special
        # --- Doors ---
        when 1    # DR Door Open Wait Close
          activate_door(linedef)
        when 26   # DR Blue Door
          activate_door(linedef, key: :blue_card)
        when 27   # DR Yellow Door
          activate_door(linedef, key: :yellow_card)
        when 28   # DR Red Door
          activate_door(linedef, key: :red_card)
        when 31   # D1 Door Open Stay
          activate_door(linedef, stay_open: true)
        when 32   # D1 Blue Door Open Stay
          activate_door(linedef, key: :blue_card, stay_open: true)
        when 33   # D1 Red Door Open Stay
          activate_door(linedef, key: :red_card, stay_open: true)
        when 34   # D1 Yellow Door Open Stay
          activate_door(linedef, key: :yellow_card, stay_open: true)
        when 103  # S1 Door Open Wait Close (tagged)
          activate_tagged_door(linedef)

        # --- Lifts ---
        when 62   # SR Lift Lower Wait Raise (repeatable)
          activate_lift(linedef)

        # --- Floor changes ---
        when 18   # S1 Raise Floor to Next Higher
          raise_floor_to_next(linedef)
        when 20   # S1 Raise Floor to Next Higher (platform)
          raise_floor_to_next(linedef)
        when 22   # W1 Raise Floor to Next Higher
          raise_floor_to_next(linedef)
        when 23   # S1 Lower Floor to Lowest
          lower_floor_to_lowest(linedef)
        when 36   # S1 Lower Floor to Highest Adjacent - 8
          lower_floor_to_highest(linedef)
        when 70   # SR Lower Floor to Highest Adjacent - 8
          lower_floor_to_highest(linedef)

        # --- Exits ---
        when 11   # S1 Exit
          @exit_triggered = :normal
        when 51   # S1 Secret Exit
          @exit_triggered = :secret

        else
          return false
        end
        true
      end

      private

      # Walk-over trigger types:
      # W1 = once, WR = repeatable
      WALK_TRIGGERS = {
        2  => :door_open_stay,    # W1 Door Open Stay
        5  => :raise_floor,       # W1 Raise Floor to Lowest Ceiling
        7  => :stairs,            # S1 Build Stairs
        8  => :stairs,            # W1 Build Stairs
        52 => :exit,              # W1 Exit
        82 => :lower_floor,       # WR Lower Floor to Lowest
        86 => :door_open_stay,    # WR Door Open Stay
        88 => :lift,              # WR Lift Lower Wait Raise
        90 => :door,              # WR Door Open Wait Close
        91 => :raise_floor,       # WR Raise Floor to Lowest Ceiling
        97 => :teleport,          # WR Teleport
        98 => :lower_floor,       # WR Lower Floor to Highest - 8
        124 => :secret_exit,      # W1 Secret Exit
      }.freeze

      # W1 types that only trigger once
      W1_TYPES = [2, 5, 7, 8, 52, 124].freeze

      def check_walk_triggers
        @near_linedefs ||= {}

        @map.linedefs.each_with_index do |ld, idx|
          next if ld.special == 0
          action = WALK_TRIGGERS[ld.special]
          next unless action

          # W1 types only trigger once
          if W1_TYPES.include?(ld.special)
            next if @crossed_linedefs[idx]
          end

          v1 = @map.vertices[ld.v1]
          v2 = @map.vertices[ld.v2]

          # Determine which side of the linedef the player is on
          # DOOM's P_CrossSpecialLine fires when the player transitions sides
          side = line_side(@player_x, @player_y, v1.x, v1.y, v2.x, v2.y)
          dist = point_line_dist(@player_x, @player_y, v1.x, v1.y, v2.x, v2.y)

          near = dist < 32  # Detection range
          prev_side = @near_linedefs[idx]

          if near && prev_side && prev_side != side
            # Player crossed the line - trigger!
            @near_linedefs[idx] = side
          elsif near && prev_side.nil?
            # First time near - record side but don't trigger yet
            @near_linedefs[idx] = side
            next
          elsif !near
            @near_linedefs[idx] = nil
            next
          else
            next  # Same side, no crossing
          end

          @crossed_linedefs[idx] = true

          case action
          when :exit
            @exit_triggered = :normal
          when :secret_exit
            @exit_triggered = :secret
          when :door_open_stay
            activate_tagged_door(ld, stay_open: true)
          when :door
            activate_tagged_door(ld)
          when :lift
            activate_lift(ld)
          when :raise_floor
            raise_floor_to_next(ld)
          when :lower_floor
            lower_floor_to_highest(ld)
          when :teleport
            teleport_player(ld)
          end
        end
      end

      # Returns which side of a line a point is on (:front or :back)
      def line_side(px, py, x1, y1, x2, y2)
        cross = (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1)
        cross >= 0 ? :front : :back
      end

      def point_line_dist(px, py, x1, y1, x2, y2)
        dx = x2 - x1; dy = y2 - y1
        len_sq = dx * dx + dy * dy
        return Math.sqrt((px - x1) ** 2 + (py - y1) ** 2) if len_sq == 0
        t = ((px - x1) * dx + (py - y1) * dy).to_f / len_sq
        t = [[t, 0.0].max, 1.0].min
        cx = x1 + t * dx; cy = y1 + t * dy
        Math.sqrt((px - cx) ** 2 + (py - cy) ** 2)
      end

      def activate_door(linedef, stay_open: false, key: nil)
        # Find the sector on the back side of the linedef
        return unless linedef.two_sided?

        back_sidedef_idx = linedef.sidedef_left
        return if back_sidedef_idx == 0xFFFF || back_sidedef_idx < 0

        back_sidedef = @map.sidedefs[back_sidedef_idx]
        sector_idx = back_sidedef.sector
        sector = @map.sectors[sector_idx]
        return unless sector

        # Check if door is already active
        if @active_doors[sector_idx]
          door = @active_doors[sector_idx]
          # If closing, reverse direction
          if door[:state] == DOOR_CLOSING
            door[:state] = DOOR_OPENING
          end
          return
        end

        # Calculate target height (find lowest adjacent ceiling)
        target_height = find_lowest_ceiling_around(sector_idx) - 4

        # Start the door
        @active_doors[sector_idx] = {
          sector: sector,
          state: DOOR_OPENING,
          target_height: target_height,
          original_height: sector.ceiling_height,
          wait_tics: 0,
          stay_open: stay_open
        }
        @sound&.door_open
      end

      def update_doors
        @active_doors.each do |sector_idx, door|
          case door[:state]
          when DOOR_OPENING
            door[:sector].ceiling_height += DOOR_SPEED
            if door[:sector].ceiling_height >= door[:target_height]
              door[:sector].ceiling_height = door[:target_height]
              if door[:stay_open]
                @active_doors.delete(sector_idx)
              else
                door[:state] = DOOR_OPEN
                door[:wait_tics] = DOOR_WAIT
              end
            end

          when DOOR_OPEN
            door[:wait_tics] -= 1
            if door[:wait_tics] <= 0
              door[:state] = DOOR_CLOSING
              @sound&.door_close
            end

          when DOOR_CLOSING
            # Check if player is in the door sector
            player_sector = @map.sector_at(@player_x, @player_y)
            if player_sector == door[:sector]
              # Player is in door - reopen it
              door[:state] = DOOR_OPENING
              next
            end

            door[:sector].ceiling_height -= DOOR_SPEED
            if door[:sector].ceiling_height <= door[:original_height]
              door[:sector].ceiling_height = door[:original_height]
              @active_doors.delete(sector_idx)
            end
          end
        end
      end

      def find_lowest_ceiling_around(sector_idx)
        lowest = Float::INFINITY

        @map.linedefs.each do |linedef|
          next unless linedef.two_sided?

          # Check if this linedef touches our sector
          right_sidedef = @map.sidedefs[linedef.sidedef_right]
          left_sidedef = @map.sidedefs[linedef.sidedef_left] if linedef.sidedef_left != 0xFFFF

          adjacent_sector = nil
          if right_sidedef&.sector == sector_idx && left_sidedef
            adjacent_sector = @map.sectors[left_sidedef.sector]
          elsif left_sidedef&.sector == sector_idx
            adjacent_sector = @map.sectors[right_sidedef.sector]
          end

          if adjacent_sector
            lowest = [lowest, adjacent_sector.ceiling_height].min
          end
        end

        lowest == Float::INFINITY ? 128 : lowest
      end

      # Door activated by tag (for S1/W1/WR tagged doors)
      def activate_tagged_door(linedef, stay_open: false)
        tag = linedef.tag
        return if tag == 0

        @map.sectors.each_with_index do |sector, idx|
          next unless sector_has_tag?(idx, tag)
          next if @active_doors[idx]

          target = find_lowest_ceiling_around(idx) - 4
          @active_doors[idx] = {
            sector: sector,
            state: DOOR_OPENING,
            target_height: target,
            original_height: sector.ceiling_height,
            wait_tics: 0,
            stay_open: stay_open,
          }
        end
        @sound&.door_open
      end

      # Lift: lower floor to lowest adjacent, wait, raise back
      def activate_lift(linedef)
        tag = linedef.tag
        return if tag == 0

        activated = false
        @map.sectors.each_with_index do |sector, idx|
          next unless sector_has_tag?(idx, tag)
          next if @active_lifts[idx]  # Already moving

          lowest = find_lowest_floor_around(idx)
          @active_lifts[idx] = {
            sector: sector,
            state: :lowering,
            target_low: lowest,
            original_height: sector.floor_height,
            wait_tics: 0,
          }
          activated = true
        end
        @sound&.platform_start if activated
      end

      def update_lifts
        @active_lifts.each do |idx, lift|
          case lift[:state]
          when :lowering
            lift[:sector].floor_height -= LIFT_SPEED
            if lift[:sector].floor_height <= lift[:target_low]
              lift[:sector].floor_height = lift[:target_low]
              lift[:state] = :waiting
              lift[:wait_tics] = LIFT_WAIT
              @sound&.platform_stop
            end
          when :waiting
            lift[:wait_tics] -= 1
            if lift[:wait_tics] <= 0
              lift[:state] = :raising
              @sound&.platform_start
            end
          when :raising
            lift[:sector].floor_height += LIFT_SPEED
            if lift[:sector].floor_height >= lift[:original_height]
              lift[:sector].floor_height = lift[:original_height]
              @active_lifts.delete(idx)
              @sound&.platform_stop
            end
          end
        end
      end

      def raise_floor_to_next(linedef)
        tag = linedef.tag
        return if tag == 0

        @map.sectors.each_with_index do |sector, idx|
          next unless sector_has_tag?(idx, tag)
          target = find_next_higher_floor(idx)
          next if target <= sector.floor_height

          @active_lifts[idx] = {
            sector: sector,
            state: :raising,
            target_low: sector.floor_height,
            original_height: target,
            wait_tics: 0,
          }
        end
      end

      def lower_floor_to_lowest(linedef)
        tag = linedef.tag
        return if tag == 0

        @map.sectors.each_with_index do |sector, idx|
          next unless sector_has_tag?(idx, tag)
          target = find_lowest_floor_around(idx)
          sector.floor_height = target
        end
      end

      def lower_floor_to_highest(linedef)
        tag = linedef.tag
        return if tag == 0

        @map.sectors.each_with_index do |sector, idx|
          next unless sector_has_tag?(idx, tag)
          target = find_highest_floor_around(idx) - 8
          sector.floor_height = target if target < sector.floor_height
        end
      end

      def teleport_player(linedef)
        tag = linedef.tag
        return if tag == 0

        # Find teleport destination thing (type 14) in tagged sector
        @map.things.each do |thing|
          next unless thing.type == 14  # Teleport destination
          sector = @map.sector_at(thing.x, thing.y)
          next unless sector
          sector_idx = @map.sectors.index(sector)
          next unless sector_has_tag?(sector_idx, tag)

          @teleport_dest = { x: thing.x, y: thing.y, angle: thing.angle }
          return
        end
      end


      def check_secrets
        # Build set of secret sector indices on first call
        @secret_sectors ||= Set.new(
          @map.sectors.each_with_index.filter_map { |s, i| i if s.special == 9 }
        )
        return if @secret_sectors.empty?

        # Find which sector the player is in via BSP subsector lookup
        subsector = @map.subsector_at(@player_x, @player_y)
        return unless subsector

        seg = @map.segs[subsector.first_seg]
        return unless seg

        ld = @map.linedefs[seg.linedef]
        return unless ld

        sd_idx = seg.direction == 0 ? ld.sidedef_right : ld.sidedef_left
        return if sd_idx == 0xFFFF

        sector_idx = @map.sidedefs[sd_idx].sector
        return if @secrets_found[sector_idx]

        if @secret_sectors.include?(sector_idx)
          @secrets_found[sector_idx] = true
          # Clear the special so it doesn't retrigger (matching Chocolate Doom)
          @map.sectors[sector_idx].special = 0
          @secret_sectors.delete(sector_idx)
        end
      end

      def sector_has_tag?(sector_idx, tag)
        @map.sectors[sector_idx].tag == tag
      end

      def find_lowest_floor_around(sector_idx)
        lowest = @map.sectors[sector_idx].floor_height
        each_adjacent_sector(sector_idx) do |adj|
          lowest = adj.floor_height if adj.floor_height < lowest
        end
        lowest
      end

      def find_highest_floor_around(sector_idx)
        highest = -32768
        each_adjacent_sector(sector_idx) do |adj|
          highest = adj.floor_height if adj.floor_height > highest
        end
        highest == -32768 ? @map.sectors[sector_idx].floor_height : highest
      end

      def find_next_higher_floor(sector_idx)
        current = @map.sectors[sector_idx].floor_height
        best = Float::INFINITY
        each_adjacent_sector(sector_idx) do |adj|
          if adj.floor_height > current && adj.floor_height < best
            best = adj.floor_height
          end
        end
        best == Float::INFINITY ? current : best
      end

      def each_adjacent_sector(sector_idx)
        @map.linedefs.each do |ld|
          next unless ld.two_sided?
          right = @map.sidedefs[ld.sidedef_right]
          left = @map.sidedefs[ld.sidedef_left] if ld.sidedef_left != 0xFFFF
          next unless left

          if right.sector == sector_idx
            yield @map.sectors[left.sector]
          elsif left.sector == sector_idx
            yield @map.sectors[right.sector]
          end
        end
      end
    end
  end
end

# frozen_string_literal: true

module Doom
  module Game
    # Animated texture/flat cycling, matching Chocolate Doom's P_InitPicAnims
    # and P_UpdateSpecials from p_spec.c.
    #
    # All animations run at 8 tics per frame (8/35 sec ≈ 0.23s).
    # Frames must be consecutive entries in the WAD; the engine uses
    # start/end names to find the range.
    class Animations
      TICS_PER_FRAME = 8

      # [is_texture, start_name, end_name]
      # From Chocolate Doom animdefs[] in p_spec.c
      ANIMDEFS = [
        # Animated flats
        [false, 'NUKAGE1',  'NUKAGE3'],
        [false, 'FWATER1',  'FWATER4'],
        [false, 'SWATER1',  'SWATER4'],
        [false, 'LAVA1',    'LAVA4'],
        [false, 'BLOOD1',   'BLOOD3'],
        [false, 'RROCK05',  'RROCK08'],
        [false, 'SLIME01',  'SLIME04'],
        [false, 'SLIME05',  'SLIME08'],
        [false, 'SLIME09',  'SLIME12'],
        # Animated wall textures
        [true, 'BLODGR1',  'BLODGR4'],
        [true, 'SLADRIP1', 'SLADRIP3'],
        [true, 'BLODRIP1', 'BLODRIP4'],
        [true, 'FIREWALA', 'FIREWALL'],
        [true, 'GSTFONT1', 'GSTFONT3'],
        [true, 'FIRELAV3', 'FIRELAVA'],
        [true, 'FIREMAG1', 'FIREMAG3'],
        [true, 'FIREBLU1', 'FIREBLU2'],
        [true, 'ROCKRED1', 'ROCKRED3'],
        [true, 'BFALL1',   'BFALL4'],
        [true, 'SFALL1',   'SFALL4'],
        [true, 'WFALL1',   'WFALL4'],
        [true, 'DBRAIN1',  'DBRAIN4'],
      ].freeze

      attr_reader :flat_translation, :texture_translation

      def initialize(texture_names, flat_names)
        @flat_translation = {}      # flat_name -> current_frame_name
        @texture_translation = {}   # texture_name -> current_frame_name
        @anims = []

        ANIMDEFS.each do |is_texture, start_name, end_name|
          names = is_texture ? texture_names : flat_names

          start_idx = names.index(start_name)
          end_idx = names.index(end_name)
          next unless start_idx && end_idx
          next if end_idx <= start_idx

          frames = names[start_idx..end_idx]
          next if frames.size < 2

          @anims << {
            is_texture: is_texture,
            frames: frames,
            speed: TICS_PER_FRAME,
          }
        end
      end

      # Call every game tic (or approximate with leveltime).
      # Matches Chocolate Doom P_UpdateSpecials:
      #   pic = basepic + ((leveltime / speed + i) % numpics)
      def update(leveltime)
        @anims.each do |anim|
          frames = anim[:frames]
          numpics = frames.size
          phase = leveltime / anim[:speed]
          translation = anim[:is_texture] ? @texture_translation : @flat_translation

          numpics.times do |i|
            current_frame = frames[(phase + i) % numpics]
            translation[frames[i]] = current_frame
          end
        end
      end

      # Translate a flat name to its current animation frame
      def translate_flat(name)
        @flat_translation[name] || name
      end

      # Translate a texture name to its current animation frame
      def translate_texture(name)
        @texture_translation[name] || name
      end
    end
  end
end

# frozen_string_literal: true

module Doom
  module Game
    # Sector light specials and scrolling walls, matching Chocolate Doom's
    # P_SpawnSpecials (p_spec.c) and p_lights.c.
    class SectorEffects
      GLOWSPEED    = 8   # Light units per tic for glow
      STROBEBRIGHT = 5   # Bright duration for strobes (tics)
      FASTDARK     = 15  # Dark duration for fast strobe (tics)
      SLOWDARK     = 35  # Dark duration for slow strobe (tics)

      def initialize(map)
        @map = map
        @effects = []
        @scroll_sides = []
        spawn_specials
      end

      # Called every game tic (35/sec)
      def update
        @effects.each(&:update)
        @scroll_sides.each { |side| side.x_offset += 1 }
      end

      private

      def spawn_specials
        @map.sectors.each do |sector|
          case sector.special
          when 1      # Flickering lights
            @effects << LightFlash.new(sector, find_min_light(sector))
          when 2      # Fast strobe
            @effects << StrobeFlash.new(sector, find_min_light(sector), FASTDARK, false)
          when 3      # Slow strobe
            @effects << StrobeFlash.new(sector, find_min_light(sector), SLOWDARK, false)
          when 4      # Fast strobe + 20% damage
            @effects << StrobeFlash.new(sector, find_min_light(sector), FASTDARK, false)
          when 8      # Glowing light
            @effects << Glow.new(sector, find_min_light(sector))
          when 12     # Sync strobe slow
            @effects << StrobeFlash.new(sector, find_min_light(sector), SLOWDARK, true)
          when 13     # Sync strobe fast
            @effects << StrobeFlash.new(sector, find_min_light(sector), FASTDARK, true)
          when 17     # Fire flicker
            @effects << FireFlicker.new(sector, find_min_light(sector))
          end
        end

        # Linedef type 48: scrolling wall (front side scrolls +1 unit/tic)
        @map.linedefs.each do |linedef|
          next unless linedef.special == 48
          side = @map.sidedefs[linedef.sidedef_right]
          @scroll_sides << side if side
        end
      end

      # P_FindMinSurroundingLight: find lowest light level among adjacent sectors
      def find_min_light(sector)
        min = sector.light_level
        sector_idx = @map.sectors.index(sector)
        return min unless sector_idx

        @map.linedefs.each do |ld|
          right = @map.sidedefs[ld.sidedef_right]
          next unless right
          left_idx = ld.sidedef_left
          next if left_idx >= 0xFFFF
          left = @map.sidedefs[left_idx]
          next unless left

          if right.sector == sector_idx && left.sector != sector_idx
            other_light = @map.sectors[left.sector].light_level
            min = other_light if other_light < min
          elsif left.sector == sector_idx && right.sector != sector_idx
            other_light = @map.sectors[right.sector].light_level
            min = other_light if other_light < min
          end
        end
        min
      end

      # T_LightFlash (type 1): mostly bright with brief random dark flickers
      class LightFlash
        def initialize(sector, minlight)
          @sector = sector
          @maxlight = sector.light_level
          @minlight = minlight
          @count = (rand(65)) + 1
        end

        def update
          @count -= 1
          return if @count > 0

          if @sector.light_level == @maxlight
            @sector.light_level = @minlight
            @count = (rand(8)) + 1       # dark for 1-8 tics
          else
            @sector.light_level = @maxlight
            @count = (rand(2) == 0 ? 1 : 65)  # bright for 1 or 65 tics (P_Random()&64)
          end
        end
      end

      # T_StrobeFlash (types 2, 3, 4, 12, 13): regular strobe blink
      class StrobeFlash
        def initialize(sector, minlight, darktime, in_sync)
          @sector = sector
          @maxlight = sector.light_level
          @minlight = minlight
          @minlight = 0 if @minlight == @maxlight
          @darktime = darktime
          @brighttime = STROBEBRIGHT
          @count = in_sync ? 1 : (rand(8)) + 1
        end

        def update
          @count -= 1
          return if @count > 0

          if @sector.light_level == @minlight
            @sector.light_level = @maxlight
            @count = @brighttime
          else
            @sector.light_level = @minlight
            @count = @darktime
          end
        end
      end

      # T_Glow (type 8): smooth triangle-wave oscillation
      class Glow
        def initialize(sector, minlight)
          @sector = sector
          @maxlight = sector.light_level
          @minlight = minlight
          @direction = -1  # start dimming
        end

        def update
          if @direction == -1
            @sector.light_level -= GLOWSPEED
            if @sector.light_level <= @minlight
              @sector.light_level += GLOWSPEED
              @direction = 1
            end
          else
            @sector.light_level += GLOWSPEED
            if @sector.light_level >= @maxlight
              @sector.light_level -= GLOWSPEED
              @direction = -1
            end
          end
        end
      end

      # T_FireFlicker (type 17): random fire-like flickering
      class FireFlicker
        def initialize(sector, minlight)
          @sector = sector
          @maxlight = sector.light_level
          @minlight = minlight + 16  # fire doesn't go as dark
          @count = 4
        end

        def update
          @count -= 1
          return if @count > 0

          amount = (rand(4)) * 16  # 0, 16, 32, or 48
          level = @maxlight - amount
          @sector.light_level = level < @minlight ? @minlight : level
          @count = 4
        end
      end
    end
  end
end

# frozen_string_literal: true

module Doom
  module Render
    SCREEN_WIDTH = 320
    SCREEN_HEIGHT = 240
    HALF_WIDTH = SCREEN_WIDTH / 2
    HALF_HEIGHT = SCREEN_HEIGHT / 2
    FOV = 90.0

    # Silhouette types for sprite clipping (matches Chocolate Doom r_defs.h)
    SIL_NONE = 0
    SIL_BOTTOM = 1
    SIL_TOP = 2
    SIL_BOTH = 3

    # Drawseg stores wall segment info for sprite clipping
    # Matches Chocolate Doom's drawseg_t structure
    Drawseg = Struct.new(:x1, :x2, :scale1, :scale2, :scalestep,
                         :silhouette, :bsilheight, :tsilheight,
                         :sprtopclip, :sprbottomclip,
                         :curline,  # seg for point-on-side test
                         :maskedtexturecol,  # per-column texture X for masked mid textures
                         :frontsector, :backsector,  # sectors for masked rendering
                         :sidedef,  # sidedef for texture lookup
                         :dist1, :dist2, :sx1, :sx2)  # for per-column distance recomputation

    # VisibleSprite stores sprite data for sorting and rendering
    # Struct is faster than Hash for fixed-field data
    VisibleSprite = Struct.new(:thing, :sprite, :view_x, :view_y, :dist, :screen_x)

    # Visplane stores floor/ceiling rendering info for a sector
    # Matches Chocolate Doom's visplane_t structure from r_plane.c
    Visplane = Struct.new(:sector, :height, :texture, :light_level, :is_ceiling,
                          :top, :bottom, :minx, :maxx) do
      def initialize(sector, height, texture, light_level, is_ceiling)
        super(sector, height, texture, light_level, is_ceiling,
              Array.new(SCREEN_WIDTH, SCREEN_HEIGHT),  # top (initially invalid)
              Array.new(SCREEN_WIDTH, -1),             # bottom (initially invalid)
              SCREEN_WIDTH,                            # minx (no columns marked yet)
              -1)                                      # maxx (no columns marked yet)
      end

      def mark(x, y1, y2)
        return if y1 > y2
        top[x] = [top[x], y1].min
        bottom[x] = [bottom[x], y2].max
        self.minx = [minx, x].min
        self.maxx = [maxx, x].max
      end

      def valid?
        minx <= maxx
      end
    end

    class Renderer
      attr_reader :framebuffer

      def initialize(wad, map, textures, palette, colormap, flats, sprites = nil, animations = nil)
        @wad = wad
        @map = map
        @textures = textures
        @palette = palette
        @colormap = colormap
        @flats = flats.to_h { |f| [f.name, f] }
        @sprites = sprites
        @animations = animations
        @hidden_things = nil
        @combat = nil
        @monster_ai = nil
        @leveltime = 0
        @skip_background_fill = false

        @framebuffer = Array.new(SCREEN_WIDTH * SCREEN_HEIGHT, 0)

        @player_x = 0.0
        @player_y = 0.0
        @player_z = 41.0
        @player_angle = 0.0
        @sin_angle = 0.0
        @cos_angle = 1.0

        # Projection constant - distance to projection plane
        @projection = HALF_WIDTH / Math.tan(FOV * Math::PI / 360.0)

        # Precomputed column data (cached based on player angle)
        @column_cos = Array.new(SCREEN_WIDTH)
        @column_sin = Array.new(SCREEN_WIDTH)
        @cached_player_angle = nil

        # Column distance scale is constant (doesn't depend on player angle)
        @column_distscale = Array.new(SCREEN_WIDTH) do |x|
          dx = x - HALF_WIDTH
          Math.sqrt(dx * dx + @projection * @projection) / @projection
        end

        # Clipping arrays
        @ceiling_clip = Array.new(SCREEN_WIDTH, -1)
        @floor_clip = Array.new(SCREEN_WIDTH, SCREEN_HEIGHT)

        # Per-sprite clip buffers, reset (not reallocated) for each draw_sprite call
        @sprite_clipbot = Array.new(SCREEN_WIDTH, -2)
        @sprite_cliptop = Array.new(SCREEN_WIDTH, -2)

        # Sprite clip arrays (copy of wall clips for sprite clipping)
        @sprite_ceiling_clip = Array.new(SCREEN_WIDTH, -1)
        @sprite_floor_clip = Array.new(SCREEN_WIDTH, SCREEN_HEIGHT)

        # Wall depth array - tracks distance to nearest wall at each column
        @wall_depth = Array.new(SCREEN_WIDTH, Float::INFINITY)
        @sprite_wall_depth = Array.new(SCREEN_WIDTH, Float::INFINITY)

        # Preallocated y_slope arrays for floor/ceiling rendering (avoids per-frame allocation)
        @y_slope_ceil = Array.new(HALF_HEIGHT + 1, 0.0)
        @y_slope_floor = Array.new(HALF_HEIGHT + 1, 0.0)
      end

      attr_reader :player_x, :player_y, :player_z, :sin_angle, :cos_angle, :framebuffer
      attr_reader :wad, :textures, :colormap, :flats, :sprites
      attr_writer :hidden_things, :combat, :monster_ai, :leveltime
      attr_accessor :skip_background_fill

      # Diagnostic: returns info about all sprites and why they are/aren't visible
      def sprite_diagnostics
        return [] unless @sprites

        results = []
        @map.things.each do |thing|
          prefix = @sprites.prefix_for(thing.type)
          next unless prefix

          info = { type: thing.type, x: thing.x, y: thing.y, prefix: prefix }

          view_x, view_y = transform_point(thing.x, thing.y)
          info[:view_x] = view_x.round(1)
          info[:view_y] = view_y.round(1)

          if view_y <= 0
            info[:status] = "behind_player"
            results << info
            next
          end

          dist = view_y
          screen_x = HALF_WIDTH + (view_x * @projection / view_y)
          info[:screen_x] = screen_x.round(1)
          info[:dist] = dist.round(1)

          dx = thing.x - @player_x
          dy = thing.y - @player_y
          angle_to_thing = Math.atan2(dy, dx)
          sprite = @sprites.get_rotated(thing.type, angle_to_thing, thing.angle)
          unless sprite
            info[:status] = "no_sprite_frame"
            results << info
            next
          end

          sprite_scale = @projection / dist
          sprite_half_width = (sprite.width * @projection / dist / 2).to_i
          info[:sprite_scale] = sprite_scale.round(3)

          if screen_x + sprite_half_width < 0
            info[:status] = "off_screen_left"
          elsif screen_x - sprite_half_width >= SCREEN_WIDTH
            info[:status] = "off_screen_right"
          else
            # Check drawseg clipping
            sprite_left = (screen_x - sprite.left_offset * sprite_scale).to_i
            sprite_right = sprite_left + (sprite.width * sprite_scale).to_i - 1
            x1 = [sprite_left, 0].max
            x2 = [sprite_right, SCREEN_WIDTH - 1].min

            sector = @map.sector_at(thing.x, thing.y)
            thing_floor = sector ? sector.floor_height : 0
            sprite_gz = thing_floor
            sprite_gzt = thing_floor + sprite.top_offset

            clipping_segs = []
            @drawsegs.reverse_each do |ds|
              next if ds.x1 > x2 || ds.x2 < x1
              next if ds.silhouette == SIL_NONE

              lowscale = [ds.scale1, ds.scale2].min
              highscale = [ds.scale1, ds.scale2].max

              if highscale < sprite_scale
                next
              elsif lowscale < sprite_scale
                next unless point_on_seg_side(thing.x, thing.y, ds.curline)
              end

              clipping_segs << {
                x1: ds.x1, x2: ds.x2,
                scale: "#{ds.scale1.round(3)}..#{ds.scale2.round(3)}",
                sil: ds.silhouette
              }
            end

            info[:screen_range] = "#{x1}..#{x2}"
            info[:clipping_segs] = clipping_segs.size
            info[:clipping_detail] = clipping_segs
            info[:status] = "visible"
          end

          results << info
        end
        results
      end

      def set_player(x, y, z, angle)
        @player_x = x.to_f
        @player_y = y.to_f
        @player_z = z.to_f
        @player_angle = angle * Math::PI / 180.0
        @sin_angle = Math.sin(@player_angle)
        @cos_angle = Math.cos(@player_angle)
      end

      def move_to(x, y)
        @player_x = x.to_f
        @player_y = y.to_f
      end

      def set_z(z)
        @player_z = z.to_f
      end

      def turn(degrees)
        @player_angle += degrees * Math::PI / 180.0
        @sin_angle = Math.sin(@player_angle)
        @cos_angle = Math.cos(@player_angle)
      end

      def render_frame
        clear_framebuffer
        reset_clipping

        @sin_angle = Math.sin(@player_angle)
        @cos_angle = Math.cos(@player_angle)

        # Precompute column angles for floor/ceiling rendering
        precompute_column_data

        # Draw player's sector floor/ceiling as background fallback.
        # Visplanes (with correct per-sector lighting) overwrite this for sectors
        # with different properties. This only remains visible at gaps between
        # same-property sectors where the light level matches anyway.
        draw_floor_ceiling_background

        # Initialize visplanes for tracking visible floor/ceiling spans
        @visplanes = []
        @visplane_hash = {}  # Hash for O(1) lookup by (height, texture, light_level, is_ceiling)

        # Initialize drawsegs for sprite clipping
        @drawsegs = []

        # Render walls via BSP traversal
        render_bsp_node(@map.nodes.size - 1)

        # Draw visplanes for sectors different from background
        draw_all_visplanes

        # Save wall clip arrays for sprite clipping (reuse preallocated arrays)
        @sprite_ceiling_clip.replace(@ceiling_clip)
        @sprite_floor_clip.replace(@floor_clip)
        @sprite_wall_depth.replace(@wall_depth)

        # R_DrawMasked: render sprites and masked middle textures interleaved
        draw_masked if @sprites
      end

      # Precompute column-based data for floor/ceiling rendering (R_InitLightTables-like)
      # Cached: only recomputes sin/cos when player angle changes
      def precompute_column_data
        return if @cached_player_angle == @player_angle

        @cached_player_angle = @player_angle

        SCREEN_WIDTH.times do |x|
          column_angle = @player_angle - Math.atan2(x - HALF_WIDTH, @projection)
          @column_cos[x] = Math.cos(column_angle)
          @column_sin[x] = Math.sin(column_angle)
        end
      end

      def draw_floor_ceiling_background
        return if @skip_background_fill
        player_sector = @map.sector_at(@player_x, @player_y)
        return unless player_sector

        fill_uncovered_with_sector(player_sector)
      end

      # Render all visplanes after BSP traversal (R_DrawPlanes in Chocolate Doom)
      def draw_all_visplanes
        @visplanes.each do |plane|
          next unless plane.valid?

          if plane.texture == 'F_SKY1'
            draw_sky_plane(plane)
          else
            render_visplane_spans(plane)
          end
        end
      end

      # Render visplane using horizontal spans (R_MakeSpans in Chocolate Doom)
      # This processes columns left-to-right, building spans and rendering them
      def render_visplane_spans(plane)
        return if plane.minx > plane.maxx

        spanstart = Array.new(SCREEN_HEIGHT)  # Track where each row's span started

        # Process columns left to right
        ((plane.minx)..(plane.maxx + 1)).each do |x|
          # Get current column bounds
          if x <= plane.maxx
            t2 = plane.top[x]
            b2 = plane.bottom[x]
            t2 = SCREEN_HEIGHT if t2 > b2  # Invalid = empty
          else
            t2, b2 = SCREEN_HEIGHT, -1  # Sentinel for final column
          end

          # Get previous column bounds
          if x > plane.minx
            t1 = plane.top[x - 1]
            b1 = plane.bottom[x - 1]
            t1 = SCREEN_HEIGHT if t1 > b1
          else
            t1, b1 = SCREEN_HEIGHT, -1
          end

          # Close spans that ended (visible in prev column, not in current)
          if t1 < SCREEN_HEIGHT
            # Rows visible in previous but not current (above current or below current)
            (t1..[b1, t2 - 1].min).each do |y|
              draw_span(plane, y, spanstart[y], x - 1) if spanstart[y]
              spanstart[y] = nil
            end
            ([t1, b2 + 1].max..b1).each do |y|
              draw_span(plane, y, spanstart[y], x - 1) if spanstart[y]
              spanstart[y] = nil
            end
          end

          # Open new spans (visible in current, not started yet)
          if t2 < SCREEN_HEIGHT
            (t2..b2).each do |y|
              spanstart[y] ||= x
            end
          end
        end
      end

      # Render one horizontal span with texture mapping (R_MapPlane in Chocolate Doom)
      def draw_span(plane, y, x1, x2)
        return if x1.nil? || x1 > x2 || y < 0 || y >= SCREEN_HEIGHT

        flat = @flats[anim_flat(plane.texture)]
        return unless flat

        # Distance from horizon (y=100 for 200-high screen)
        dy = y - HALF_HEIGHT
        return if dy == 0

        # Plane height relative to player eye level
        plane_height = (plane.height - @player_z).abs
        return if plane_height == 0

        # Perpendicular distance to this row: distance = height * projection / dy
        perp_dist = plane_height * @projection / dy.abs

        # Calculate lighting for this distance
        light = calculate_flat_light(plane.light_level, perp_dist)
        cmap = @colormap.maps[light]

        # Cache locals for inner loop
        framebuffer = @framebuffer
        column_distscale = @column_distscale
        column_cos = @column_cos
        column_sin = @column_sin
        player_x = @player_x
        neg_player_y = -@player_y
        row_offset = y * SCREEN_WIDTH
        flat_pixels = flat.pixels

        # Clamp to screen bounds
        x1 = 0 if x1 < 0
        x2 = SCREEN_WIDTH - 1 if x2 >= SCREEN_WIDTH

        # Draw each pixel in the span using while loop
        x = x1
        while x <= x2
          ray_dist = perp_dist * column_distscale[x]
          tex_x = (player_x + ray_dist * column_cos[x]).to_i & 63
          tex_y = (neg_player_y - ray_dist * column_sin[x]).to_i & 63
          color = flat_pixels[tex_y * 64 + tex_x]
          framebuffer[row_offset + x] = cmap[color]
          x += 1
        end
      end

      # Render sky ceiling as columns (column-based like walls, not spans)
      # Sky texture mid-point: texel row at screen center.
      # Chocolate Doom: skytexturemid = SCREENHEIGHT/2 = 100 (for 200px screen).
      # Scale factor: our 240px screen maps to DOOM's 200px sky coordinates.
      SKY_TEXTUREMID = 100.0
      SKY_YSCALE = 200.0 / SCREEN_HEIGHT  # 0.833 - maps our pixels to DOOM's 200px space
      # Chocolate Doom: ANGLETOSKYSHIFT=22 gives 4 sky repetitions per 360 degrees.
      # Full circle (2pi) * 512/pi = 1024 columns, masked to 256 = 4 repetitions.
      SKY_XSCALE = 512.0 / Math::PI

      def draw_sky_plane(plane)
        sky_texture = @textures['SKY1']
        return unless sky_texture

        framebuffer = @framebuffer
        player_angle = @player_angle
        projection = @projection
        sky_width = sky_texture.width
        sky_height = sky_texture.height

        # Clamp to screen bounds
        minx = [plane.minx, 0].max
        maxx = [plane.maxx, SCREEN_WIDTH - 1].min

        (minx..maxx).each do |x|
          y1 = plane.top[x]
          y2 = plane.bottom[x]
          next if y1 > y2

          # Clamp y bounds
          y1 = 0 if y1 < 0
          y2 = SCREEN_HEIGHT - 1 if y2 >= SCREEN_HEIGHT

          # Sky X: 4 repetitions per 360 degrees (matching ANGLETOSKYSHIFT=22)
          column_angle = player_angle - Math.atan2(x - HALF_WIDTH, projection)
          sky_x = (column_angle * SKY_XSCALE).to_i % sky_width
          column = sky_texture.column_pixels(sky_x)
          next unless column

          # Sky Y: texel = skytexturemid + (y - centery) * scale
          # Maps texel 0 to screen top, texel 100 to horizon (1:1 for DOOM's 200px)
          (y1..y2).each do |y|
            tex_y = (SKY_TEXTUREMID + (y - HALF_HEIGHT) * SKY_YSCALE).to_i % sky_height
            color = column[tex_y]
            framebuffer[y * SCREEN_WIDTH + x] = color
          end
        end
      end

      def find_or_create_visplane(sector, height, texture, light_level, is_ceiling)
        # O(1) hash lookup instead of O(n) linear search
        key = [height, texture, light_level, is_ceiling]
        plane = @visplane_hash[key]

        unless plane
          plane = Visplane.new(sector, height, texture, light_level, is_ceiling)
          @visplanes << plane
          @visplane_hash[key] = plane
        end

        plane
      end

      # R_CheckPlane equivalent - check if columns in range are already marked
      # If overlap exists, create a new visplane; otherwise update minx/maxx
      def check_plane(plane, start_x, stop_x)
        return plane unless plane

        # Calculate intersection and union of column ranges
        if start_x < plane.minx
          intrl = plane.minx
          unionl = start_x
        else
          unionl = plane.minx
          intrl = start_x
        end

        if stop_x > plane.maxx
          intrh = plane.maxx
          unionh = stop_x
        else
          unionh = plane.maxx
          intrh = stop_x
        end

        # Check if any column in intersection range is already marked
        # A column is marked if top[x] <= bottom[x] (valid range)
        overlap = false
        (intrl..intrh).each do |x|
          next if x < 0 || x >= SCREEN_WIDTH
          if plane.top[x] <= plane.bottom[x]
            overlap = true
            break
          end
        end

        if !overlap
          # No overlap - reuse same visplane with expanded range
          plane.minx = unionl if unionl < plane.minx
          plane.maxx = unionh if unionh > plane.maxx
          return plane
        end

        # Overlap detected - create new visplane with same properties
        new_plane = Visplane.new(
          plane.sector,
          plane.height,
          plane.texture,
          plane.light_level,
          plane.is_ceiling
        )
        new_plane.minx = start_x
        new_plane.maxx = stop_x
        @visplanes << new_plane

        # Update hash to point to the new plane (for subsequent lookups)
        key = [plane.height, plane.texture, plane.light_level, plane.is_ceiling]
        @visplane_hash[key] = new_plane

        new_plane
      end

      def fill_uncovered_with_sector(default_sector)
        # Cache all instance variables as locals for faster access
        framebuffer = @framebuffer
        column_cos = @column_cos
        column_sin = @column_sin
        column_distscale = @column_distscale
        projection = @projection
        player_angle = @player_angle
        player_x = @player_x
        neg_player_y = -@player_y

        ceil_height = (default_sector.ceiling_height - @player_z).abs
        floor_height = (default_sector.floor_height - @player_z).abs
        ceil_flat = @flats[anim_flat(default_sector.ceiling_texture)]
        floor_flat = @flats[anim_flat(default_sector.floor_texture)]
        ceil_pixels = ceil_flat&.pixels
        floor_pixels = floor_flat&.pixels
        is_sky = default_sector.ceiling_texture == 'F_SKY1'
        sky_texture = is_sky ? @textures['SKY1'] : nil
        light_level = default_sector.light_level
        colormap_maps = @colormap.maps

        # Compute y_slope for each row (perpendicular distance) - reuse preallocated arrays
        y_slope_ceil = @y_slope_ceil
        y_slope_floor = @y_slope_floor
        (1..HALF_HEIGHT).each do |dy|
          y_slope_ceil[dy] = ceil_height * projection / dy.to_f
          y_slope_floor[dy] = floor_height * projection / dy.to_f
        end

        # Draw ceiling (rows 0 to HALF_HEIGHT-1) using while loops for speed
        y = 0
        while y < HALF_HEIGHT
          dy = HALF_HEIGHT - y
          if dy > 0
            perp_dist = y_slope_ceil[dy]
            if perp_dist > 0
              light = calculate_flat_light(light_level, perp_dist)
              cmap = colormap_maps[light]
              row_offset = y * SCREEN_WIDTH

              if is_sky && sky_texture
                sky_w = sky_texture.width
                sky_h = sky_texture.height
                sky_y = (SKY_TEXTUREMID + (y - HALF_HEIGHT) * SKY_YSCALE).to_i % sky_h
                x = 0
                while x < SCREEN_WIDTH
                  column_angle = player_angle - Math.atan2(x - HALF_WIDTH, projection)
                  sky_x = (column_angle * SKY_XSCALE).to_i % sky_w
                  color = sky_texture.column_pixels(sky_x)[sky_y]
                  framebuffer[row_offset + x] = color
                  x += 1
                end
              elsif ceil_flat
                x = 0
                while x < SCREEN_WIDTH
                  ray_dist = perp_dist * column_distscale[x]
                  tex_x = (player_x + ray_dist * column_cos[x]).to_i & 63
                  tex_y = (neg_player_y - ray_dist * column_sin[x]).to_i & 63
                  color = ceil_pixels[tex_y * 64 + tex_x]
                  framebuffer[row_offset + x] = cmap[color]
                  x += 1
                end
              end
            end
          end
          y += 1
        end

        # Draw floor (rows HALF_HEIGHT to SCREEN_HEIGHT-1)
        y = HALF_HEIGHT
        while y < SCREEN_HEIGHT
          dy = y - HALF_HEIGHT
          if dy > 0
            perp_dist = y_slope_floor[dy]
            if perp_dist > 0
              light = calculate_flat_light(light_level, perp_dist)
              cmap = colormap_maps[light]
              row_offset = y * SCREEN_WIDTH

              if floor_flat
                x = 0
                while x < SCREEN_WIDTH
                  ray_dist = perp_dist * column_distscale[x]
                  tex_x = (player_x + ray_dist * column_cos[x]).to_i & 63
                  tex_y = (neg_player_y - ray_dist * column_sin[x]).to_i & 63
                  color = floor_pixels[tex_y * 64 + tex_x]
                  framebuffer[row_offset + x] = cmap[color]
                  x += 1
                end
              end
            end
          end
          y += 1
        end
      end

      private

      # Translate flat/texture names through animation system
      def anim_flat(name)
        @animations ? @animations.translate_flat(name) : name
      end

      def anim_texture(name)
        @animations ? @animations.translate_texture(name) : name
      end

      def clear_framebuffer
        @framebuffer.fill(0)
      end

      def reset_clipping
        @ceiling_clip.fill(-1)
        @floor_clip.fill(SCREEN_HEIGHT)
        @wall_depth.fill(Float::INFINITY)
      end

      def render_bsp_node(node_index)
        if node_index & Map::Node::SUBSECTOR_FLAG != 0
          render_subsector(node_index & ~Map::Node::SUBSECTOR_FLAG)
          return
        end

        node = @map.nodes[node_index]
        side = point_on_side(@player_x, @player_y, node)

        if side == 0
          render_bsp_node(node.child_right)
          back_bbox = node.bbox_left
          render_bsp_node(node.child_left) if check_bbox(back_bbox)
        else
          render_bsp_node(node.child_left)
          back_bbox = node.bbox_right
          render_bsp_node(node.child_right) if check_bbox(back_bbox)
        end
      end

      # R_CheckBBox - check if a bounding box is potentially visible.
      # Projects the bbox corners to screen columns and checks if any
      # column in that range is not fully occluded.
      def check_bbox(bbox)
        # Transform all 4 corners to view space
        near = 1.0
        all_in_front = true
        min_sx = SCREEN_WIDTH
        max_sx = -1

        [[bbox.left, bbox.bottom], [bbox.right, bbox.bottom],
         [bbox.left, bbox.top], [bbox.right, bbox.top]].each do |wx, wy|
          vx, vy = transform_point(wx, wy)

          if vy < near
            # Any corner behind the near plane - bbox is too close to cull safely
            all_in_front = false
          else
            sx = HALF_WIDTH + (vx * @projection / vy)
            sx_i = sx.to_i
            min_sx = sx_i if sx_i < min_sx
            max_sx = sx_i if sx_i > max_sx
          end
        end

        # If any corner is behind the near plane, conservatively assume visible.
        # Only cull when all 4 corners are cleanly in front and we can check occlusion.
        return true unless all_in_front

        min_sx = 0 if min_sx < 0
        max_sx = SCREEN_WIDTH - 1 if max_sx >= SCREEN_WIDTH
        return false if min_sx > max_sx

        # Check if any column in the range is not fully occluded
        x = min_sx
        while x <= max_sx
          return true if @ceiling_clip[x] < @floor_clip[x] - 1
          x += 1
        end

        false
      end

      def point_on_side(x, y, node)
        dx = x - node.x
        dy = y - node.y
        left = dy * node.dx
        right = dx * node.dy
        right >= left ? 0 : 1
      end

      def render_subsector(index)
        subsector = @map.subsectors[index]
        return unless subsector

        # Get the sector for this subsector (from first seg's linedef)
        first_seg = @map.segs[subsector.first_seg]
        linedef = @map.linedefs[first_seg.linedef]
        sidedef_idx = first_seg.direction == 0 ? linedef.sidedef_right : linedef.sidedef_left
        return if sidedef_idx < 0

        sidedef = @map.sidedefs[sidedef_idx]
        @current_sector = @map.sectors[sidedef.sector]

        # Create floor visplane if floor is visible (below eye level)
        # Matches Chocolate Doom: if (frontsector->floorheight < viewz)
        if @current_sector.floor_height < @player_z
          @current_floor_plane = find_or_create_visplane(
            @current_sector,
            @current_sector.floor_height,
            @current_sector.floor_texture,
            @current_sector.light_level,
            false
          )
        else
          @current_floor_plane = nil
        end

        # Create ceiling visplane if ceiling is visible (above eye level or sky)
        # Matches Chocolate Doom: if (frontsector->ceilingheight > viewz || frontsector->ceilingpic == skyflatnum)
        is_sky = @current_sector.ceiling_texture == 'F_SKY1'
        if @current_sector.ceiling_height > @player_z || is_sky
          @current_ceiling_plane = find_or_create_visplane(
            @current_sector,
            @current_sector.ceiling_height,
            @current_sector.ceiling_texture,
            @current_sector.light_level,
            true
          )
        else
          @current_ceiling_plane = nil
        end

        # Process all segs in this subsector
        subsector.seg_count.times do |i|
          seg = @map.segs[subsector.first_seg + i]
          render_seg(seg)
        end
      end

      def render_seg(seg)
        v1 = @map.vertices[seg.v1]
        v2 = @map.vertices[seg.v2]

        # Transform vertices to view space
        # View space: +Y is forward, +X is right
        x1, y1 = transform_point(v1.x, v1.y)
        x2, y2 = transform_point(v2.x, v2.y)

        # Both behind player?
        return if y1 <= 0 && y2 <= 0

        # Clip to near plane
        near = 1.0
        if y1 < near || y2 < near
          if y1 < near && y2 < near
            return
          elsif y1 < near
            t = (near - y1) / (y2 - y1)
            x1 = x1 + t * (x2 - x1)
            y1 = near
          elsif y2 < near
            t = (near - y1) / (y2 - y1)
            x2 = x1 + t * (x2 - x1)
            y2 = near
          end
        end

        # Project to screen X using Doom's tangent-based approach
        # screenX = centerX + viewX * projection / viewY
        # This is equivalent to Doom's: centerX - tan(angle) * focalLength
        # because tan(angle) = -viewX / viewY in our coordinate convention
        sx1 = HALF_WIDTH + (x1 * @projection / y1)
        sx2 = HALF_WIDTH + (x2 * @projection / y2)

        # Backface: seg spanning from right to left means we see the back
        return if sx1 >= sx2

        # Off screen?
        return if sx2 < 0 || sx1 >= SCREEN_WIDTH

        # Clamp to screen
        x1i = [sx1.to_i, 0].max
        x2i = [sx2.to_i, SCREEN_WIDTH - 1].min
        return if x1i > x2i

        # Get sector info
        linedef = @map.linedefs[seg.linedef]
        sidedef_idx = seg.direction == 0 ? linedef.sidedef_right : linedef.sidedef_left
        return if sidedef_idx < 0

        sidedef = @map.sidedefs[sidedef_idx]
        sector = @map.sectors[sidedef.sector]

        # Back sector for two-sided lines
        back_sector = nil
        if linedef.two_sided?
          back_sidedef_idx = seg.direction == 0 ? linedef.sidedef_left : linedef.sidedef_right
          if back_sidedef_idx >= 0
            back_sidedef = @map.sidedefs[back_sidedef_idx]
            back_sector = @map.sectors[back_sidedef.sector]
          end
        end

        # Calculate seg length for texture mapping
        seg_v1 = @map.vertices[seg.v1]
        seg_v2 = @map.vertices[seg.v2]
        seg_length = Math.sqrt((seg_v2.x - seg_v1.x)**2 + (seg_v2.y - seg_v1.y)**2)

        draw_seg_range(x1i, x2i, sx1, sx2, y1, y2, sector, back_sector, sidedef, linedef, seg, seg_length)
      end

      def transform_point(wx, wy)
        # Translate
        dx = wx - @player_x
        dy = wy - @player_y

        # Rotate - transform world to view space
        # View space: +Y forward (in direction of angle), +X is right
        x = dx * @sin_angle - dy * @cos_angle
        y = dx * @cos_angle + dy * @sin_angle

        [x, y]
      end

      # Determine which side of a seg a point is on (R_PointOnSegSide from Chocolate Doom)
      # Returns true if point is on back side, false if on front side
      def point_on_seg_side(x, y, seg)
        v1 = @map.vertices[seg.v1]
        v2 = @map.vertices[seg.v2]

        lx = v1.x
        ly = v1.y
        ldx = v2.x - lx
        ldy = v2.y - ly

        # Handle axis-aligned lines
        if ldx == 0
          return x <= lx ? ldy > 0 : ldy < 0
        end
        if ldy == 0
          return y <= ly ? ldx < 0 : ldx > 0
        end

        dx = x - lx
        dy = y - ly

        # Cross product to determine side
        left = ldy * dx
        right = dy * ldx

        right >= left
      end

      def draw_seg_range(x1, x2, sx1, sx2, dist1, dist2, sector, back_sector, sidedef, linedef, seg, seg_length)
        seg_v1 = @map.vertices[seg.v1]
        seg_v2 = @map.vertices[seg.v2]

        # Precompute ray-seg intersection coefficients for per-column texture mapping.
        # For screen column x, the view ray direction in world space is:
        #   ray = (sin_a * dx_col + cos_a * proj, -cos_a * dx_col + sin_a * proj)
        # where dx_col = x - HALF_WIDTH.
        # The ray-seg intersection parameter s (position along seg) is:
        #   s = (E * x + F) / (A * x + B)
        # where A, B, E, F are precomputed per-seg constants.
        seg_dx = (seg_v2.x - seg_v1.x).to_f
        seg_dy = (seg_v2.y - seg_v1.y).to_f
        px = @player_x - seg_v1.x.to_f
        py = @player_y - seg_v1.y.to_f
        sin_a = @sin_angle
        cos_a = @cos_angle
        proj = @projection

        c1 = cos_a * proj - sin_a * HALF_WIDTH
        c2 = sin_a * proj + cos_a * HALF_WIDTH

        # denom(x) = seg_dy * ray_dx - seg_dx * ray_dy = A * x + B
        tex_a = seg_dy * sin_a + seg_dx * cos_a
        tex_b = seg_dy * c1 - seg_dx * c2

        # numer(x) = py * ray_dx - px * ray_dy = E * x + F
        tex_e = py * sin_a + px * cos_a
        tex_f = py * c1 - px * c2

        tex_offset = seg.offset + sidedef.x_offset

        # Calculate scales for drawseg (scale = projection / distance)
        scale1 = dist1 > 0 ? @projection / dist1 : Float::INFINITY
        scale2 = dist2 > 0 ? @projection / dist2 : Float::INFINITY

        # Determine silhouette type for sprite clipping
        silhouette = SIL_NONE
        bsilheight = 0  # bottom silhouette world height
        tsilheight = 0  # top silhouette world height

        if back_sector
          # Two-sided line - check for silhouettes
          if sector.floor_height > back_sector.floor_height
            silhouette |= SIL_BOTTOM
            bsilheight = sector.floor_height
          elsif back_sector.floor_height > @player_z
            silhouette |= SIL_BOTTOM
            bsilheight = Float::INFINITY
          end

          if sector.ceiling_height < back_sector.ceiling_height
            silhouette |= SIL_TOP
            tsilheight = sector.ceiling_height
          elsif back_sector.ceiling_height < @player_z
            silhouette |= SIL_TOP
            tsilheight = -Float::INFINITY
          end
        else
          # Solid wall - full silhouette
          silhouette = SIL_BOTH
          bsilheight = Float::INFINITY
          tsilheight = -Float::INFINITY
        end

        # Detect masked middle texture (grates, bars, fences)
        has_masked = false
        @current_masked_cols = nil
        if back_sector && sidedef
          mid_tex = sidedef.middle_texture
          if mid_tex && mid_tex != '-' && !mid_tex.empty?
            has_masked = true
            @current_masked_cols = Array.new(x2 - x1 + 1)
            # Force silhouette so sprites get clipped against this seg
            if (silhouette & SIL_TOP) == 0
              silhouette |= SIL_TOP
              tsilheight = -Float::INFINITY
            end
            if (silhouette & SIL_BOTTOM) == 0
              silhouette |= SIL_BOTTOM
              bsilheight = Float::INFINITY
            end
          end
        end

        # Check planes for this seg range (R_CheckPlane equivalent)
        if @current_floor_plane
          @current_floor_plane = check_plane(@current_floor_plane, x1, x2)
        end
        if @current_ceiling_plane
          @current_ceiling_plane = check_plane(@current_ceiling_plane, x1, x2)
        end

        (x1..x2).each do |x|
          next if @ceiling_clip[x] >= @floor_clip[x] - 1

          # Screen-space interpolation for distance
          t = sx2 != sx1 ? (x - sx1) / (sx2 - sx1) : 0
          t = t.clamp(0.0, 1.0)

          if dist1 > 0 && dist2 > 0
            inv_dist = (1.0 - t) / dist1 + t / dist2
            dist = 1.0 / inv_dist
          else
            dist = dist1 > 0 ? dist1 : dist2
          end

          # Ray-seg intersection for texture column
          # s = (E * x + F) / (A * x + B) gives position along seg in world units
          denom = tex_a * x + tex_b
          if denom.abs < 0.001
            s = 0.0
          else
            s = (tex_e * x + tex_f) / denom
          end
          tex_col = (tex_offset + s * seg_length).to_i

          # Skip if too close
          next if dist < 1

          # Scale factor for this column
          scale = @projection / dist

          # World heights relative to player eye level
          front_floor = sector.floor_height - @player_z
          front_ceil = sector.ceiling_height - @player_z

          # Project to screen Y (Y increases downward on screen)
          # Chocolate Doom rounds ceiling UP and floor DOWN to avoid 1-pixel gaps:
          #   yl = (topfrac+HEIGHTUNIT-1)>>HEIGHTBITS  (ceil for front ceiling)
          #   yh = bottomfrac>>HEIGHTBITS              (floor for front floor)
          front_ceil_y = (HALF_HEIGHT - front_ceil * scale).ceil
          front_floor_y = (HALF_HEIGHT - front_floor * scale).to_i

          # Clamp to current clip bounds
          ceil_y = [front_ceil_y, @ceiling_clip[x] + 1].max
          floor_y = [front_floor_y, @floor_clip[x] - 1].min

          if back_sector
            # Two-sided line
            back_floor = back_sector.floor_height - @player_z
            back_ceil = back_sector.ceiling_height - @player_z

            # Chocolate Doom rounding:
            #   pixhigh>>HEIGHTBITS (truncate for back ceiling / upper wall end)
            #   (pixlow+HEIGHTUNIT-1)>>HEIGHTBITS (ceil for back floor / lower wall start)
            back_ceil_y = (HALF_HEIGHT - back_ceil * scale).to_i
            back_floor_y = (HALF_HEIGHT - back_floor * scale).ceil

            # Determine visible ceiling/floor boundaries (the opening between sectors)
            # high_ceil = top of the opening on screen (max Y = lower world ceiling)
            # low_floor = bottom of the opening on screen (min Y = higher world floor)
            high_ceil = [ceil_y, back_ceil_y].max
            low_floor = [floor_y, back_floor_y].min

            # Check for closed door (no opening between sectors)
            # Matches Chocolate Doom: backsector->ceilingheight <= frontsector->floorheight
            #                      || backsector->floorheight >= frontsector->ceilingheight
            closed_door = back_sector.ceiling_height <= sector.floor_height ||
                          back_sector.floor_height >= sector.ceiling_height

            both_sky = sector.ceiling_texture == 'F_SKY1' && back_sector.ceiling_texture == 'F_SKY1'

            # Determine whether to mark ceiling/floor visplanes.
            # Match Chocolate Doom: only mark when properties differ, plus force
            # for closed doors. The background fill covers same-property gaps.
            # Chocolate Doom sky hack: "worldtop = worldhigh" makes the front
            # ceiling equal to the back ceiling when both are sky. This prevents
            # the low sky ceiling from clipping walls in adjacent sectors.
            effective_front_ceil = both_sky ? back_sector.ceiling_height : sector.ceiling_height

            if closed_door
              should_mark_ceiling = true
              should_mark_floor = true
            else
              should_mark_ceiling = effective_front_ceil != back_sector.ceiling_height ||
                                    sector.ceiling_texture != back_sector.ceiling_texture ||
                                    sector.light_level != back_sector.light_level
              should_mark_floor = sector.floor_height != back_sector.floor_height ||
                                  sector.floor_texture != back_sector.floor_texture ||
                                  sector.light_level != back_sector.light_level
            end

            # Mark ceiling visplane
            if @current_ceiling_plane && should_mark_ceiling
              mark_top = @ceiling_clip[x] + 1
              mark_bottom = ceil_y - 1
              mark_bottom = [@floor_clip[x] - 1, mark_bottom].min
              if mark_top <= mark_bottom
                @current_ceiling_plane.mark(x, mark_top, mark_bottom)
              end
            end

            # Mark floor visplane
            if @current_floor_plane && should_mark_floor
              mark_top = floor_y + 1
              mark_bottom = @floor_clip[x] - 1
              mark_top = [@ceiling_clip[x] + 1, mark_top].max
              if mark_top <= mark_bottom
                @current_floor_plane.mark(x, mark_top, mark_bottom)
              end
            end

            # Upper wall (ceiling step down) - skip if both sectors have sky
            if !both_sky && sector.ceiling_height > back_sector.ceiling_height
              if linedef.upper_unpegged?
                upper_tex_y = sidedef.y_offset
              else
                texture = @textures[sidedef.upper_texture]
                tex_height = texture ? texture.height : 128
                upper_tex_y = sidedef.y_offset + back_sector.ceiling_height - sector.ceiling_height + tex_height
              end
              draw_wall_column_ex(x, ceil_y, back_ceil_y, sidedef.upper_texture, dist,
                                  sector.light_level, tex_col, upper_tex_y, scale, sector.ceiling_height)
            end

            # Lower wall (floor step up)
            if sector.floor_height < back_sector.floor_height
              if linedef.lower_unpegged?
                lower_tex_y = sidedef.y_offset + sector.ceiling_height - back_sector.floor_height
              else
                lower_tex_y = sidedef.y_offset
              end
              draw_wall_column_ex(x, back_floor_y, floor_y, sidedef.lower_texture, dist,
                                  sector.light_level, tex_col, lower_tex_y, scale, back_sector.floor_height)
            end

            # Store masked texture column for deferred rendering (grates, bars)
            if @current_masked_cols
              @current_masked_cols[x - x1] = tex_col
            end

            # Update clip bounds
            if closed_door
              @wall_depth[x] = [@wall_depth[x], dist].min
              @ceiling_clip[x] = SCREEN_HEIGHT
              @floor_clip[x] = -1
            else
              # Ceiling clip (uses effective_front_ceil for sky hack)
              if effective_front_ceil > back_sector.ceiling_height
                @ceiling_clip[x] = [back_ceil_y, @ceiling_clip[x]].max
              elsif effective_front_ceil < back_sector.ceiling_height
                @ceiling_clip[x] = [ceil_y - 1, @ceiling_clip[x]].max
              elsif sector.ceiling_texture != back_sector.ceiling_texture ||
                    sector.light_level != back_sector.light_level
                @ceiling_clip[x] = [ceil_y - 1, @ceiling_clip[x]].max
              end

              # Floor clip
              if sector.floor_height < back_sector.floor_height
                @floor_clip[x] = [back_floor_y, @floor_clip[x]].min
              elsif sector.floor_height > back_sector.floor_height
                # Chocolate Doom: else if (markfloor) floorclip = yh + 1
                @floor_clip[x] = [floor_y + 1, @floor_clip[x]].min
              elsif sector.floor_texture != back_sector.floor_texture ||
                    sector.light_level != back_sector.light_level
                @floor_clip[x] = [floor_y + 1, @floor_clip[x]].min
              end
            end
          else
            # One-sided (solid) wall
            # Mark ceiling visplane (from previous clip to wall's ceiling)
            # Clamp to floor_clip to prevent ceiling bleeding through portal openings
            if @current_ceiling_plane
              mark_top = @ceiling_clip[x] + 1
              mark_bottom = [ceil_y - 1, @floor_clip[x] - 1].min
              if mark_top <= mark_bottom
                @current_ceiling_plane.mark(x, mark_top, mark_bottom)
              end
            end

            # Mark floor visplane (from wall's floor to previous floor clip)
            # Clamp to ceiling_clip to prevent floor bleeding through portal openings
            if @current_floor_plane
              mark_top = [floor_y + 1, @ceiling_clip[x] + 1].max
              mark_bottom = @floor_clip[x] - 1
              if mark_top <= mark_bottom
                @current_floor_plane.mark(x, mark_top, mark_bottom)
              end
            end

            # Draw wall (from clipped ceiling to clipped floor)
            # Middle texture Y offset depends on DONTPEGBOTTOM flag
            # With DONTPEGBOTTOM: texture bottom aligns with floor
            # Without: texture top aligns with ceiling
            if linedef.lower_unpegged?
              texture = @textures[sidedef.middle_texture]
              tex_height = texture ? texture.height : 128
              mid_tex_y = sidedef.y_offset + tex_height - (sector.ceiling_height - sector.floor_height)
            else
              mid_tex_y = sidedef.y_offset
            end
            draw_wall_column_ex(x, ceil_y, floor_y, sidedef.middle_texture, dist,
                                sector.light_level, tex_col, mid_tex_y, scale, sector.ceiling_height)

            # Track wall depth for sprite clipping (solid wall occludes this column)
            @wall_depth[x] = [@wall_depth[x], dist].min

            # Fully occluded
            @ceiling_clip[x] = SCREEN_HEIGHT
            @floor_clip[x] = -1
          end
        end

        # Save drawseg for sprite clipping and masked rendering
        if silhouette != SIL_NONE || has_masked
          sprtopclip = @ceiling_clip[x1..x2].dup
          sprbottomclip = @floor_clip[x1..x2].dup
          scalestep = (x2 > x1) ? (scale2 - scale1) / (x2 - x1) : 0

          drawseg = Drawseg.new(
            x1, x2,
            scale1, scale2, scalestep,
            silhouette,
            bsilheight, tsilheight,
            sprtopclip, sprbottomclip,
            seg,
            has_masked ? @current_masked_cols : nil,
            sector, back_sector, sidedef,
            dist1, dist2, sx1.to_f, sx2.to_f
          )
          @drawsegs << drawseg
        end
        @current_masked_cols = nil
      end

      # Wall column drawing with proper texture mapping
      # tex_col: texture column (X coordinate in texture)
      # tex_y_start: starting Y coordinate in texture (accounts for pegging)
      # scale: projection scale for this column (projection / distance)
      # world_top: world height of the top of this wall section
      def draw_wall_column_ex(x, y1, y2, texture_name, dist, light_level, tex_col, tex_y_start, scale, world_top)
        return if y1 > y2
        return if texture_name.nil? || texture_name.empty? || texture_name == '-'

        # Clip to visible range (ceiling_clip/floor_clip)
        clip_top = @ceiling_clip[x] + 1
        clip_bottom = @floor_clip[x] - 1
        y1 = [y1, clip_top].max
        y2 = [y2, clip_bottom].min
        return if y1 > y2

        texture = @textures[anim_texture(texture_name)]
        return unless texture

        light = calculate_light(light_level, dist)
        cmap = @colormap.maps[light]
        framebuffer = @framebuffer
        tex_width = texture.width
        tex_height = texture.height

        # Texture X coordinate (wrap around texture width)
        tex_x = tex_col.to_i % tex_width

        # Get the column of pixels
        column = texture.column_pixels(tex_x)
        return unless column

        # Texture step per screen pixel
        tex_step = 1.0 / scale

        # Calculate where the unclipped wall top would be on screen
        unclipped_y1 = HALF_HEIGHT - (world_top - @player_z) * scale

        # Adjust tex_y_start for any clipping at the top
        tex_y_at_y1 = tex_y_start + (y1 - unclipped_y1) * tex_step

        # Clamp to screen bounds
        y1 = 0 if y1 < 0
        y2 = SCREEN_HEIGHT - 1 if y2 >= SCREEN_HEIGHT

        # Draw wall column using while loop
        y = y1
        while y <= y2
          screen_offset = y - y1
          tex_y = (tex_y_at_y1 + screen_offset * tex_step).to_i % tex_height

          color = column[tex_y]
          framebuffer[y * SCREEN_WIDTH + x] = cmap[color] if color
          y += 1
        end
      end

      # Cache which textures have transparent pixels
      def texture_has_transparency?(texture)
        @transparency_cache ||= {}
        name = texture.name
        return @transparency_cache[name] if @transparency_cache.key?(name)
        @transparency_cache[name] = texture.width.times.any? do |x|
          col = texture.column_pixels(x)
          col&.any?(&:nil?)
        end
      end

      # Draw a deferred masked column using per-drawseg clip values saved at BSP time
      def draw_wall_column_masked_deferred(md)
        x = md[:x]
        tex_name = md[:tex]
        return if tex_name.nil? || tex_name.empty? || tex_name == '-'

        # Use saved clips (from BSP time, before far walls closed them)
        # AND final sprite clips (to prevent far grates drawing over near walls)
        # Take the tightest combination of both
        y1 = [md[:clip_top] + 1, @sprite_ceiling_clip[x] + 1, md[:y1]].max
        y2 = [md[:clip_bottom] - 1, @sprite_floor_clip[x] - 1, md[:y2]].min
        return if y1 > y2

        # Depth test: far grate behind a near solid wall
        return if md[:dist] > @sprite_wall_depth[x]

        texture = @textures[anim_texture(tex_name)]
        return unless texture

        light = calculate_light(md[:light], md[:dist])
        cmap = @colormap.maps[light]
        framebuffer = @framebuffer
        tex_width = texture.width
        tex_height = texture.height

        tex_x = md[:tex_col].to_i % tex_width
        column = texture.column_pixels(tex_x)
        return unless column

        scale = md[:scale]
        tex_step = 1.0 / scale
        unclipped_y1 = HALF_HEIGHT - (md[:world_top] - @player_z) * scale
        tex_y_at_y1 = md[:tex_y] + (y1 - unclipped_y1) * tex_step

        y1 = 0 if y1 < 0
        y2 = SCREEN_HEIGHT - 1 if y2 >= SCREEN_HEIGHT

        y = y1
        while y <= y2
          screen_offset = y - y1
          tex_y = (tex_y_at_y1 + screen_offset * tex_step).to_i % tex_height
          color = column[tex_y]
          framebuffer[y * SCREEN_WIDTH + x] = cmap[color] if color
          y += 1
        end
      end

      # Draw a masked (transparent) wall column for middle textures on two-sided linedefs.
      # Unlike draw_wall_column_ex, skips nil pixels (transparent areas).
      def draw_wall_column_masked(x, y1, y2, texture_name, dist, light_level, tex_col, tex_y_start, scale, world_top)
        return if y1 > y2
        return if texture_name.nil? || texture_name.empty? || texture_name == '-'

        clip_top = @ceiling_clip[x] + 1
        clip_bottom = @floor_clip[x] - 1
        y1 = [y1, clip_top].max
        y2 = [y2, clip_bottom].min
        return if y1 > y2

        texture = @textures[anim_texture(texture_name)]
        return unless texture

        light = calculate_light(light_level, dist)
        cmap = @colormap.maps[light]
        framebuffer = @framebuffer
        tex_width = texture.width
        tex_height = texture.height

        tex_x = tex_col.to_i % tex_width
        column = texture.column_pixels(tex_x)
        return unless column

        tex_step = 1.0 / scale
        unclipped_y1 = HALF_HEIGHT - (world_top - @player_z) * scale
        tex_y_at_y1 = tex_y_start + (y1 - unclipped_y1) * tex_step

        y1 = 0 if y1 < 0
        y2 = SCREEN_HEIGHT - 1 if y2 >= SCREEN_HEIGHT

        y = y1
        while y <= y2
          screen_offset = y - y1
          tex_y = (tex_y_at_y1 + screen_offset * tex_step).to_i % tex_height
          color = column[tex_y]
          # Skip transparent pixels (nil in patch data)
          if color
            framebuffer[y * SCREEN_WIDTH + x] = cmap[color]
          end
          y += 1
        end
      end

      # Calculate colormap index for lighting
      # Doom uses: walllights[scale >> LIGHTSCALESHIFT] where scale = projection/distance
      # LIGHTSCALESHIFT = 12, MAXLIGHTSCALE = 48, NUMCOLORMAPS = 32
      # Doom lighting constants from r_main.h
      LIGHTLEVELS = 16
      LIGHTSEGSHIFT = 4
      MAXLIGHTSCALE = 48
      LIGHTSCALESHIFT = 12
      MAXLIGHTZ = 128
      LIGHTZSHIFT = 20
      NUMCOLORMAPS = 32
      DISTMAP = 2

      # Calculate light for walls using Doom's scalelight formula
      # In Doom: scalelight[lightnum][scale_index] where:
      #   lightnum = sector_light >> LIGHTSEGSHIFT (0-15)
      #   startmap = (LIGHTLEVELS-1-lightnum) * 2 * NUMCOLORMAPS / LIGHTLEVELS
      #   level = startmap - scale_index * SCREENWIDTH / (viewwidth * DISTMAP)
      #   scale_index = rw_scale >> LIGHTSCALESHIFT (0-47)
      def calculate_light(light_level, dist)
        # lightnum from sector light level (0-15)
        lightnum = (light_level >> LIGHTSEGSHIFT).clamp(0, LIGHTLEVELS - 1)

        # startmap = (15 - lightnum) * 4 for LIGHTLEVELS=16, NUMCOLORMAPS=32
        startmap = ((LIGHTLEVELS - 1 - lightnum) * 2 * NUMCOLORMAPS) / LIGHTLEVELS

        # scale_index from projection scale
        # rw_scale (fixed point) = projection * FRACUNIT / distance
        # scale_index = rw_scale >> LIGHTSCALESHIFT = projection * 16 / distance
        # With projection = 160: scale_index = 2560 / distance
        scale_index = dist > 0 ? (2560.0 / dist).to_i : MAXLIGHTSCALE
        scale_index = scale_index.clamp(0, MAXLIGHTSCALE - 1)

        # level = startmap - scale_index * 320 / (320 * 2) = startmap - scale_index / 2
        level = startmap - (scale_index * SCREEN_WIDTH / (SCREEN_WIDTH * DISTMAP))

        level.clamp(0, NUMCOLORMAPS - 1)
      end

      # Calculate light for floor/ceiling using Doom's zlight formula
      # In Doom: zlight[lightnum][z_index] where:
      #   z_index = distance >> LIGHTZSHIFT (0-127)
      #   For each z_index, a scale is computed and used to find the level
      def calculate_flat_light(light_level, distance)
        # lightnum from sector light level (0-15)
        lightnum = (light_level >> LIGHTSEGSHIFT).clamp(0, LIGHTLEVELS - 1)

        # startmap = (LIGHTLEVELS-1-lightnum)*2*NUMCOLORMAPS/LIGHTLEVELS
        startmap = ((LIGHTLEVELS - 1 - lightnum) * 2 * NUMCOLORMAPS) / LIGHTLEVELS

        # z_index = distance (in fixed point) >> LIGHTZSHIFT
        # Our float distance * FRACUNIT >> LIGHTZSHIFT = distance * 65536 / 1048576 = distance / 16
        z_index = (distance / 16.0).to_i.clamp(0, MAXLIGHTZ - 1)

        # From R_InitLightTables: scale = FixedDiv(160*FRACUNIT, (j+1)<<LIGHTZSHIFT)
        #   = (160*65536*65536) / ((j+1)*1048576) = 655360 / (j+1)
        # level = startmap - scale/FRACUNIT = startmap - 655360/65536/(j+1) = startmap - 10/(j+1)
        diminish = 10.0 / (z_index + 1)

        level = startmap - diminish
        level.to_i.clamp(0, NUMCOLORMAPS - 1)
      end

      def render_sprites
        return unless @sprites

        # Collect visible sprites with their distances
        visible_sprites = []

        @map.things.each_with_index do |thing, thing_idx|
          # Skip picked-up items
          next if @hidden_things && @hidden_things[thing_idx]

          # Check if we have a sprite for this thing type
          next unless @sprites.prefix_for(thing.type)

          # Transform to view space
          view_x, view_y = transform_point(thing.x, thing.y)

          # Skip if behind player
          next if view_y <= 0

          # Calculate distance for sorting and scaling
          dist = view_y

          # Calculate angle from player to thing (for rotation selection)
          dx = thing.x - @player_x
          dy = thing.y - @player_y
          angle_to_thing = Math.atan2(dy, dx)

          # Get sprite: death frame > walking frame > idle frame
          if @combat && @combat.dead?(thing_idx)
            sprite = @combat.death_sprite(thing_idx, thing.type, angle_to_thing, thing.angle)
            next unless sprite  # Barrel disappeared after explosion
          elsif @monster_ai
            mon = @monster_ai.monster_by_thing_idx[thing_idx]
            if mon && mon.active
              if mon.attacking
                # Attack animation: show attack frames (E, F, G...)
                prefix = @sprites.prefix_for(thing.type)
                atk_frames = Game::MonsterAI::ATTACK_FRAMES[prefix]
                if atk_frames
                  frame_idx = (mon.attack_frame_tic / Game::MonsterAI::ATTACK_FRAME_TICS).clamp(0, atk_frames.size - 1)
                  sprite = @sprites.get_frame(thing.type, atk_frames[frame_idx], angle_to_thing, thing.angle)
                end
              else
                # Walking animation: cycle through frames A-D
                walk_frame = %w[A B C D][@leveltime / 4 % 4]
                sprite = @sprites.get_frame(thing.type, walk_frame, angle_to_thing, thing.angle)
              end
            end
            sprite ||= @sprites.get_rotated(thing.type, angle_to_thing, thing.angle)
          else
            sprite = @sprites.get_rotated(thing.type, angle_to_thing, thing.angle)
          end
          next unless sprite

          # Project to screen X
          screen_x = HALF_WIDTH + (view_x * @projection / view_y)

          # Skip if completely off screen (with margin for sprite width)
          sprite_half_width = (sprite.width * @projection / dist / 2).to_i
          next if screen_x + sprite_half_width < 0
          next if screen_x - sprite_half_width >= SCREEN_WIDTH

          visible_sprites << VisibleSprite.new(thing, sprite, view_x, view_y, dist, screen_x)
        end

        # Sort by distance (back to front for proper overdraw)
        visible_sprites.sort_by! { |s| -s.dist }

        # Draw each sprite with drawseg interleaving
        visible_sprites.each do |vs|
          draw_sprite_with_masking(vs)
        end

        # Draw remaining masked segs not triggered by sprites
        @drawsegs.reverse_each do |ds|
          next unless ds.maskedtexturecol
          render_masked_seg_range(ds, ds.x1, ds.x2)
        end

        # Draw projectiles, explosions, and bullet puffs
        if @combat
          render_projectiles
          render_puffs
        end
      end

      # Chocolate Doom R_DrawMasked: interleave sprites with masked drawsegs
      def draw_masked
        render_sprites
      end

      # Draw sprite, rendering masked drawsegs behind it first (R_DrawSprite)
      def draw_sprite_with_masking(spr)
        sprite_scale = @projection / spr.dist

        # Scan drawsegs newest to oldest (nearest to farthest)
        @drawsegs.reverse_each do |ds|
          next if ds.x1 > spr.screen_x.to_i + spr.sprite.width || ds.x2 < spr.screen_x.to_i - spr.sprite.width
          next if ds.silhouette == SIL_NONE && ds.maskedtexturecol.nil?

          ds_max_scale = [ds.scale1, ds.scale2].max
          ds_min_scale = [ds.scale1, ds.scale2].min

          # Is the drawseg behind the sprite?
          if ds_max_scale < sprite_scale
            # Draw masked texture behind the sprite
            if ds.maskedtexturecol
              r1 = [ds.x1, spr.screen_x.to_i - spr.sprite.width].max
              r2 = [ds.x2, spr.screen_x.to_i + spr.sprite.width].min
              render_masked_seg_range(ds, r1, r2)
            end
            next  # Don't clip against things behind
          end
        end

        draw_sprite(spr)
      end

      # Chocolate Doom R_RenderMaskedSegRange
      def render_masked_seg_range(ds, rx1, rx2)
        return unless ds.maskedtexturecol
        return unless ds.sidedef

        mid_tex_name = ds.sidedef.middle_texture
        return if mid_tex_name.nil? || mid_tex_name == '-' || mid_tex_name.empty?

        texture = @textures[anim_texture(mid_tex_name)]
        return unless texture

        front = ds.frontsector
        back = ds.backsector
        return unless front && back

        # Texture anchoring (matching Chocolate Doom)
        if ds.curline && @map.linedefs[ds.curline.linedef].lower_unpegged?
          higher_floor = [front.floor_height, back.floor_height].max
          dc_texturemid = higher_floor + texture.height - @player_z
        else
          lower_ceiling = [front.ceiling_height, back.ceiling_height].min
          dc_texturemid = lower_ceiling - @player_z
        end
        dc_texturemid += ds.sidedef.y_offset

        light_level = front.light_level

        # Iterate columns
        rx1 = [rx1, ds.x1].max
        rx2 = [rx2, ds.x2].min

        (rx1..rx2).each do |x|
          col_idx = x - ds.x1
          next if col_idx < 0 || col_idx >= ds.maskedtexturecol.size

          tex_col = ds.maskedtexturecol[col_idx]
          next unless tex_col  # nil = already drawn or empty

          # Per-column distance using same 1/z interpolation as wall renderer
          if ds.dist1 && ds.dist2 && ds.dist1 > 0 && ds.dist2 > 0 && ds.sx2 != ds.sx1
            t = ((x - ds.sx1) / (ds.sx2 - ds.sx1)).clamp(0.0, 1.0)
            inv_dist = (1.0 - t) / ds.dist1 + t / ds.dist2
            col_dist = 1.0 / inv_dist
          else
            col_dist = ds.dist1 || 1.0
          end
          next if col_dist < 1
          spryscale = @projection / col_dist
          spryscale = [spryscale, 64.0].min

          # Screen Y of texture top
          sprtopscreen = HALF_HEIGHT - dc_texturemid * spryscale

          # Get clip bounds from drawseg
          clip_idx = x - ds.x1
          mceilingclip = ds.sprtopclip[clip_idx]
          mfloorclip = ds.sprbottomclip[clip_idx]

          # Calculate column draw range
          tex_x = tex_col.to_i % texture.width
          column = texture.column_pixels(tex_x)
          next unless column

          iscale = 1.0 / spryscale

          light = calculate_light(light_level, col_dist)
          cmap = @colormap.maps[light]

          # Draw each non-nil run of pixels (transparency)
          tex_height = texture.height
          y_start = nil
          (0..tex_height).each do |ty|
            color = ty < tex_height ? column[ty] : nil
            if color
              y_start = ty unless y_start
            elsif y_start
              # Draw run from y_start to ty-1
              top_y = (sprtopscreen + y_start * spryscale).to_i
              bot_y = (sprtopscreen + ty * spryscale).to_i - 1
              top_y = [top_y, mceilingclip + 1].max
              bot_y = [bot_y, mfloorclip - 1].min

              if top_y <= bot_y
                top_y = [top_y, 0].max
                bot_y = [bot_y, SCREEN_HEIGHT - 1].min
                tex_frac = y_start + (top_y - (sprtopscreen + y_start * spryscale)) * iscale
                y = top_y
                while y <= bot_y
                  t = ((tex_frac).to_i % tex_height)
                  c = column[t]
                  @framebuffer[y * SCREEN_WIDTH + x] = cmap[c] if c
                  tex_frac += iscale
                  y += 1
                end
              end
              y_start = nil
            end
          end

          # Mark column as drawn
          ds.maskedtexturecol[col_idx] = nil
        end
      end

      # Stub for projectiles/explosions that carries a z height
      ProjectileStub = Struct.new(:x, :y, :angle, :type, :flags, :z)

      def render_projectiles
        # Render all projectiles in flight
        @combat.projectiles.each do |proj|
          view_x, view_y = transform_point(proj.x, proj.y)
          next if view_y <= 0

          prefix = proj.sprite_prefix || 'MISL'

          # Try rotation-based sprite first (rockets, baron fireballs)
          rocket_angle = Math.atan2(proj.dy, proj.dx)
          viewer_angle = Math.atan2(proj.y - @player_y, proj.x - @player_x)
          angle_diff = (viewer_angle - rocket_angle) % (2 * Math::PI)
          rotation = ((angle_diff + Math::PI / 8) / (Math::PI / 4)).to_i % 8 + 1

          # Animate flight: cycle A/B frames
          flight_frame = %w[A B][@leveltime / 4 % 2]
          proj_sprite = @sprites.send(:load_sprite_frame, prefix, flight_frame, rotation)
          proj_sprite ||= @sprites.send(:load_sprite_frame, prefix, flight_frame, 0)
          proj_sprite ||= @sprites.send(:load_sprite_frame, prefix, 'A', 0)
          next unless proj_sprite

          screen_x = HALF_WIDTH + (view_x * @projection / view_y)
          stub = ProjectileStub.new(proj.x, proj.y, 0, 0, 0, proj.z)
          visible = VisibleSprite.new(stub, proj_sprite, view_x, view_y, view_y, screen_x)
          draw_sprite(visible)
        end

        # Render explosions at projectile height
        @combat.explosions.each do |expl|
          view_x, view_y = transform_point(expl[:x], expl[:y])
          next if view_y <= 0
          elapsed = (@combat.instance_variable_get(:@tic) - expl[:tic])
          frame_idx = (elapsed / 4).clamp(0, 2)

          prefix = expl[:sprite] || 'MISL'
          frame_letter = (prefix == 'MISL') ? %w[B C D][frame_idx] : %w[C D E][frame_idx]

          expl_sprite = @sprites.send(:load_sprite_frame, prefix, frame_letter, 0)
          next unless expl_sprite
          screen_x = HALF_WIDTH + (view_x * @projection / view_y)
          expl_z = expl[:z] || @player_z
          stub = ProjectileStub.new(expl[:x], expl[:y], 0, 0, 0, expl_z)
          visible = VisibleSprite.new(stub, expl_sprite, view_x, view_y, view_y, screen_x)
          draw_sprite(visible)
        end
      end

      def render_puffs
        @combat.puffs.each do |puff|
          view_x, view_y = transform_point(puff[:x], puff[:y])
          next if view_y <= 0
          elapsed = @combat.instance_variable_get(:@tic) - puff[:tic]
          frame_idx = (elapsed / 3).clamp(0, 3)
          frame_letter = %w[A B C D][frame_idx]
          puff_sprite = @sprites.send(:load_sprite_frame, 'PUFF', frame_letter, 0)
          next unless puff_sprite
          screen_x = HALF_WIDTH + (view_x * @projection / view_y)
          stub = ProjectileStub.new(puff[:x], puff[:y], 0, 0, 0, puff[:z])
          visible = VisibleSprite.new(stub, puff_sprite, view_x, view_y, view_y, screen_x)
          draw_sprite(visible)
        end
      end

      def draw_sprite(vs)
        sprite = vs.sprite
        dist = vs.dist
        screen_x = vs.screen_x
        thing = vs.thing

        # Calculate scale (inverse of distance, used for depth comparison)
        sprite_scale = @projection / dist

        # Sprite dimensions on screen
        sprite_screen_width = (sprite.width * sprite_scale).to_i
        sprite_screen_height = (sprite.height * sprite_scale).to_i

        return if sprite_screen_width <= 0 || sprite_screen_height <= 0

        # Get sector for lighting and Z position
        sector = @map.sector_at(thing.x, thing.y)
        light_level = sector ? sector.light_level : 160
        light = calculate_light(light_level, dist)

        # Sprite screen bounds
        sprite_left = (screen_x - sprite.left_offset * sprite_scale).to_i
        sprite_right = sprite_left + sprite_screen_width - 1

        # Clamp to screen
        x1 = [sprite_left, 0].max
        x2 = [sprite_right, SCREEN_WIDTH - 1].min
        return if x1 > x2

        # Calculate sprite world Z positions (matching Chocolate Doom R_ProjectSprite)
        # gzt = mobj->z + spritetopoffset (top of sprite)
        # gz = gzt - spriteheight (bottom of sprite, 1:1 pixel:unit)
        thing_floor = sector ? sector.floor_height : 0
        if thing.is_a?(ProjectileStub) && thing.z
          base_z = thing.z
        else
          base_z = thing_floor
        end
        sprite_gzt = base_z + sprite.top_offset
        sprite_gz = sprite_gzt - sprite.height

        # Reuse per-frame clip buffers (-2 = not yet clipped). Only reset the
        # range we'll touch ([x1, x2]); leftover values past x2 are ignored.
        clipbot = @sprite_clipbot
        cliptop = @sprite_cliptop
        x1.upto(x2) do |i|
          clipbot[i] = -2
          cliptop[i] = -2
        end

        # Scan drawsegs from back to front for obscuring segs
        @drawsegs.reverse_each do |ds|
          # Skip if drawseg doesn't overlap sprite horizontally
          next if ds.x1 > x2 || ds.x2 < x1

          # Skip if drawseg has no silhouette
          next if ds.silhouette == SIL_NONE

          # Determine overlap range
          r1 = [ds.x1, x1].max
          r2 = [ds.x2, x2].min

          # Get drawseg's scale range for depth comparison
          lowscale = [ds.scale1, ds.scale2].min
          highscale = [ds.scale1, ds.scale2].max

          # Chocolate Doom logic: skip if seg is behind sprite
          # If highscale < sprite_scale: wall entirely behind sprite
          # OR if lowscale < sprite_scale AND sprite is on front side of wall
          if highscale < sprite_scale
            next
          elsif lowscale < sprite_scale
            # Partial overlap - check if sprite is in front of the seg
            next unless point_on_seg_side(thing.x, thing.y, ds.curline)
          end

          # Determine which silhouettes apply based on sprite Z
          silhouette = ds.silhouette

          # If sprite bottom is at or above the bottom silhouette height, don't clip bottom
          if sprite_gz >= ds.bsilheight
            silhouette &= ~SIL_BOTTOM
          end

          # If sprite top is at or below the top silhouette height, don't clip top
          if sprite_gzt <= ds.tsilheight
            silhouette &= ~SIL_TOP
          end

          # Apply clipping for each column in the overlap
          (r1..r2).each do |x|
            # Index into drawseg's clip arrays (0-based from ds.x1)
            ds_idx = x - ds.x1

            if (silhouette & SIL_BOTTOM) != 0 && clipbot[x] == -2
              clipbot[x] = ds.sprbottomclip[ds_idx] if ds.sprbottomclip && ds_idx < ds.sprbottomclip.length
            end

            if (silhouette & SIL_TOP) != 0 && cliptop[x] == -2
              cliptop[x] = ds.sprtopclip[ds_idx] if ds.sprtopclip && ds_idx < ds.sprtopclip.length
            end
          end
        end

        # Fill in default values for unclipped columns
        (x1..x2).each do |x|
          clipbot[x] = SCREEN_HEIGHT if clipbot[x] == -2
          cliptop[x] = -1 if cliptop[x] == -2
        end

        # Calculate sprite screen Y positions
        sprite_top_world = sprite_gzt - @player_z
        sprite_bottom_world = sprite_gz - @player_z
        sprite_top_screen = (HALF_HEIGHT - sprite_top_world * sprite_scale).to_i
        sprite_bottom_screen = (HALF_HEIGHT - sprite_bottom_world * sprite_scale).to_i

        # Cache for inner loop
        framebuffer = @framebuffer
        cmap = @colormap.maps[light]
        sprite_width = sprite.width
        sprite_height = sprite.height

        # Draw each column of the sprite
        (x1..x2).each do |x|
          # Get clip bounds for this column
          top_clip = cliptop[x] + 1
          bottom_clip = clipbot[x] - 1

          next if top_clip > bottom_clip

          # Calculate which texture column to use
          tex_x = ((x - sprite_left) * sprite_width / sprite_screen_width).to_i
          tex_x = tex_x.clamp(0, sprite_width - 1)

          # Get column pixels
          column = sprite.column_pixels(tex_x)
          next unless column

          # Draw visible portion of this column
          y_start = [sprite_top_screen, top_clip].max
          y_end = [sprite_bottom_screen, bottom_clip].min

          (y_start..y_end).each do |y|
            # Calculate texture Y
            tex_y = ((y - sprite_top_screen) * sprite_height / sprite_screen_height).to_i
            tex_y = tex_y.clamp(0, sprite_height - 1)

            # Get pixel (nil = transparent)
            color = column[tex_y]
            next unless color

            # Apply lighting and write directly to framebuffer
            framebuffer[y * SCREEN_WIDTH + x] = cmap[color]
          end
        end
      end
    end
  end
end

# frozen_string_literal: true

module Doom
  module Render
    # Renders the classic DOOM status bar at the bottom of the screen
    class StatusBar
      STATUS_BAR_HEIGHT = 32
      STATUS_BAR_Y = SCREEN_HEIGHT - STATUS_BAR_HEIGHT

      # DOOM status bar layout (from st_stuff.c)
      # Positions from Chocolate Doom st_stuff.c (relative to status bar top)
      AMMO_RIGHT_X = 44      # ST_AMMOX - right edge of 3-digit ammo
      HEALTH_RIGHT_X = 90    # ST_HEALTHX
      ARMOR_RIGHT_X = 221    # ST_ARMORX

      ARMS_BG_X = 104        # ST_ARMSBGX
      ARMS_BG_Y = 0          # ST_ARMSBGY (relative to status bar)
      ARMS_X = 111            # ST_ARMSX
      ARMS_Y = 4              # ST_ARMSY (relative to status bar)
      ARMS_XSPACE = 12
      ARMS_YSPACE = 10

      FACE_X = 149           # Centered in face background area
      FACE_Y = 2             # Vertically centered in status bar

      KEYS_X = 239           # ST_KEY0X

      # Small ammo counts (right side of status bar)
      SMALL_AMMO_X = 288     # Current ammo X
      SMALL_MAX_X = 314      # Max ammo X
      SMALL_AMMO_Y = [5, 11, 23, 17]  # Bullets, Shells, Cells, Rockets (relative to bar)

      NUM_WIDTH = 14          # Width of large digit
      SMALL_NUM_WIDTH = 4     # Width of small digit

      attr_reader :gfx

      def initialize(hud_graphics, player_state)
        @gfx = hud_graphics
        @player = player_state
        @face_timer = 0
        @face_index = 0
      end

      def render(framebuffer)
        # Draw status bar background
        draw_sprite(framebuffer, @gfx.status_bar, 0, STATUS_BAR_Y) if @gfx.status_bar

        # Draw arms background (single-player only, replaces FRAG area)
        draw_sprite(framebuffer, @gfx.arms_background, ARMS_BG_X, STATUS_BAR_Y + ARMS_BG_Y) if @gfx.arms_background

        # Y position for numbers (3 pixels from top of status bar)
        num_y = STATUS_BAR_Y + 3

        # Draw ammo count (right-aligned ending at AMMO_RIGHT_X)
        draw_number_right(framebuffer, @player.current_ammo, AMMO_RIGHT_X, num_y) if @player.current_ammo

        # Draw health with percent
        draw_number_right(framebuffer, @player.health, HEALTH_RIGHT_X, num_y)
        draw_percent(framebuffer, HEALTH_RIGHT_X, num_y)

        # Draw weapon selector (2-7)
        draw_arms(framebuffer)

        # Draw face
        draw_face(framebuffer)

        # Draw armor with percent
        draw_number_right(framebuffer, @player.armor, ARMOR_RIGHT_X, num_y)
        draw_percent(framebuffer, ARMOR_RIGHT_X, num_y)

        # Draw keys
        draw_keys(framebuffer)

        # Draw small ammo counts (right side)
        draw_ammo_counts(framebuffer)
      end

      def update
        # Cycle face animation
        @face_timer += 1
        if @face_timer > 15  # Change face every ~0.5 seconds
          @face_timer = 0
          @face_index = (@face_index + 1) % 3
        end
      end

      private

      def draw_sprite(framebuffer, sprite, x, y)
        return unless sprite

        sprite.width.times do |sx|
          column = sprite.column_pixels(sx)
          next unless column

          draw_x = x + sx
          next if draw_x < 0 || draw_x >= SCREEN_WIDTH

          column.each_with_index do |color, sy|
            next unless color

            draw_y = y + sy
            next if draw_y < 0 || draw_y >= SCREEN_HEIGHT

            framebuffer[draw_y * SCREEN_WIDTH + draw_x] = color
          end
        end
      end

      # Draw number right-aligned with right edge at right_x
      def draw_number_right(framebuffer, value, right_x, y)
        return unless value

        value = value.to_i.clamp(-999, 999)
        str = value.to_s

        # Draw from right to left, starting from right edge
        current_x = right_x
        str.reverse.each_char do |char|
          digit_sprite = if char == '-'
                           @gfx.numbers['-']
                         else
                           @gfx.numbers[char.to_i]
                         end

          if digit_sprite
            current_x -= NUM_WIDTH
            draw_sprite(framebuffer, digit_sprite, current_x, y)
          end
        end
      end

      def draw_percent(framebuffer, x, y)
        percent = @gfx.numbers['%']
        draw_sprite(framebuffer, percent, x, y) if percent
      end

      def draw_arms(framebuffer)
        # Weapon numbers 2-7 in a 3x2 grid
        6.times do |i|
          weapon_num = i + 2  # weapons 2-7
          owned = @player.has_weapons[weapon_num]
          digit = owned ? @gfx.yellow_numbers[weapon_num] : @gfx.grey_numbers[weapon_num]
          next unless digit

          x = ARMS_X + (i % 3) * ARMS_XSPACE
          y = STATUS_BAR_Y + ARMS_Y + (i / 3) * ARMS_YSPACE
          draw_sprite(framebuffer, digit, x, y)
        end
      end

      def draw_face(framebuffer)
        # Pain level: 0 = healthy, 4 = near death
        health = @player.health.clamp(0, 100)
        pain_level = ((100 - health) * 5) / 101

        face = if @player.health <= 0
                 @gfx.faces[:dead]
               else
                 faces = @gfx.faces[pain_level]
                 faces[:straight][@face_index] if faces && faces[:straight]
               end

        return unless face
        draw_sprite(framebuffer, face, FACE_X, STATUS_BAR_Y + FACE_Y)
      end

      def draw_ammo_counts(framebuffer)
        ammo_current = [@player.ammo_bullets, @player.ammo_shells, @player.ammo_cells, @player.ammo_rockets]
        ammo_max = [@player.max_bullets, @player.max_shells, @player.max_cells, @player.max_rockets]

        4.times do |i|
          y = STATUS_BAR_Y + SMALL_AMMO_Y[i]
          draw_small_number_right(framebuffer, ammo_current[i], SMALL_AMMO_X, y)
          draw_small_number_right(framebuffer, ammo_max[i], SMALL_MAX_X, y)
        end
      end

      def draw_small_number_right(framebuffer, value, right_x, y)
        return unless value
        str = value.to_i.to_s
        current_x = right_x
        str.reverse.each_char do |char|
          digit = @gfx.yellow_numbers[char.to_i]
          if digit
            current_x -= SMALL_NUM_WIDTH
            draw_sprite(framebuffer, digit, current_x, y)
          end
        end
      end

      def draw_keys(framebuffer)
        key_x = KEYS_X
        key_spacing = 10

        # Blue keys (top row)
        if @player.keys[:blue_card]
          draw_sprite(framebuffer, @gfx.keys[:blue_card], key_x, STATUS_BAR_Y + 3)
        elsif @player.keys[:blue_skull]
          draw_sprite(framebuffer, @gfx.keys[:blue_skull], key_x, STATUS_BAR_Y + 3)
        end

        # Yellow keys (middle row)
        if @player.keys[:yellow_card]
          draw_sprite(framebuffer, @gfx.keys[:yellow_card], key_x, STATUS_BAR_Y + 13)
        elsif @player.keys[:yellow_skull]
          draw_sprite(framebuffer, @gfx.keys[:yellow_skull], key_x, STATUS_BAR_Y + 13)
        end

        # Red keys (bottom row)
        if @player.keys[:red_card]
          draw_sprite(framebuffer, @gfx.keys[:red_card], key_x, STATUS_BAR_Y + 23)
        elsif @player.keys[:red_skull]
          draw_sprite(framebuffer, @gfx.keys[:red_skull], key_x, STATUS_BAR_Y + 23)
        end
      end
    end
  end
end

# frozen_string_literal: true

module Doom
  module Render
    # Renders the first-person weapon view
    class WeaponRenderer
      # Weapon is rendered above the status bar
      WEAPON_AREA_HEIGHT = SCREEN_HEIGHT - StatusBar::STATUS_BAR_HEIGHT

      # Chocolate Doom R_DrawPSprite weapon positioning:
      # centery = viewheight/2 (view area excluding status bar)
      # texturemid = centery - (WEAPONTOP - spritetopoffset)
      # dc_yl = centery - texturemid (first visible row)
      WEAPONTOP = 32
      VIEW_CENTERY = (SCREEN_HEIGHT - StatusBar::STATUS_BAR_HEIGHT) / 2  # 104

      attr_reader :gfx

      def initialize(hud_graphics, player_state)
        @gfx = hud_graphics
        @player = player_state
      end

      def render(framebuffer)
        weapon_name = @player.weapon_name
        weapon_data = @gfx.weapons[weapon_name]
        return unless weapon_data

        # Get the appropriate frame
        sprite = if @player.attacking && weapon_data[:fire]&.any?
                   frame = @player.attack_frame.clamp(0, weapon_data[:fire].length - 1)
                   weapon_data[:fire][frame]
                 else
                   weapon_data[:idle]
                 end

        return unless sprite

        # Bob offset (frozen during attack to keep weapon steady)
        bob_x = @player.attacking ? 0 : @player.weapon_bob_x.to_i
        bob_y = @player.attacking ? 0 : @player.weapon_bob_y.to_i

        # Chocolate Doom on 200px: dc_yl = WEAPONTOP - topoffset
        # Our view area is 208px (240-32 status bar) vs DOOM's 168px (200-32)
        # Offset by half the extra height to keep weapon centered in view
        x = 1 - sprite.left_offset + bob_x
        y = WEAPONTOP - sprite.top_offset + 20 + bob_y

        draw_weapon_sprite(framebuffer, sprite, x, y)

        # Draw muzzle flash only on the first fire frame (the actual shot)
        if @player.attacking && @player.attack_frame == 0
          draw_muzzle_flash(framebuffer, weapon_name)
        end
      end

      private

      def draw_weapon_sprite(framebuffer, sprite, base_x, base_y)
        return unless sprite

        # Clip to screen bounds (don't draw over status bar)
        max_y = WEAPON_AREA_HEIGHT - 1

        sprite.width.times do |sx|
          column = sprite.column_pixels(sx)
          next unless column

          draw_x = base_x + sx
          next if draw_x < 0 || draw_x >= SCREEN_WIDTH

          column.each_with_index do |color, sy|
            next unless color  # Skip transparent pixels

            draw_y = base_y + sy
            next if draw_y < 0 || draw_y > max_y

            framebuffer[draw_y * SCREEN_WIDTH + draw_x] = color
          end
        end
      end

      def draw_muzzle_flash(framebuffer, weapon_name)
        weapon_data = @gfx.weapons[weapon_name]
        return unless weapon_data && weapon_data[:flash]

        flash_frame = @player.attack_frame.clamp(0, weapon_data[:flash].length - 1)
        flash_sprite = weapon_data[:flash][flash_frame]
        return unless flash_sprite

        # Flash uses same positioning as weapon sprite (built-in offsets)
        # Same positioning formula as weapon sprite
        flash_x = 1 - flash_sprite.left_offset
        flash_y = WEAPONTOP - flash_sprite.top_offset + 20

        draw_weapon_sprite(framebuffer, flash_sprite, flash_x, flash_y)
      end
    end
  end
end

# --- interactive driver ------------------------------------------------------
# Protocol, one byte in / one frame out, so the host stays in control of pacing:
#   in   'w' 's' forward/back, 'a' 'd' strafe, 'j' 'l' turn, 'q' quit,
#        '.'     no input this frame
#   out  SCREEN_WIDTH * SCREEN_HEIGHT bytes, palette indices
wad      = Doom::Wad::Reader.new(WAD_PATH)
palette  = Doom::Wad::Palette.load(wad)
colormap = Doom::Wad::Colormap.load(wad)
flats    = Doom::Wad::Flat.load_all(wad)
textures = Doom::Wad::TextureManager.new(wad)
sprites  = Doom::Wad::SpriteManager.new(wad)
map      = Doom::Map::MapData.load(wad, 'E1M1')
renderer = Doom::Render::Renderer.new(wad, map, textures, palette, colormap, flats, sprites)
renderer.skip_background_fill = true

ps = map.player_start
px = ps.x.to_f
py = ps.y.to_f
pz = 41.0
pa = ps.angle.to_f          # degrees, as the renderer wants

# The palette is the one the page needs to colour the indices; hand it over
# once, before any frame, as 768 bytes after a 4-byte marker.
pal = palette.respond_to?(:colors) ? palette.colors : nil
if pal
  bytes = []
  pal.each do |c|
    if c.is_a?(Array)
      bytes << (c[0] & 0xff) << (c[1] & 0xff) << (c[2] & 0xff)
    else
      bytes << ((c >> 16) & 0xff) << ((c >> 8) & 0xff) << (c & 0xff)
    end
  end
  bytes = bytes[0, 768]
  bytes << 0 while bytes.length < 768
  $stdout.write("PAL0")
  $stdout.write(bytes.pack("C*"))
end

STEP = 12.0
TURN = 5.0
DEG  = 3.14159265358979 / 180.0

loop do
  cmd = STDIN.read(1)
  break if cmd.nil? || cmd == "q"
  case cmd
  when "w", "s"
    d = (cmd == "w") ? STEP : -STEP
    px += Math.cos(pa * DEG) * d
    py += Math.sin(pa * DEG) * d
  when "a", "d"
    d = (cmd == "a") ? STEP : -STEP
    px += Math.cos((pa + 90.0) * DEG) * d
    py += Math.sin((pa + 90.0) * DEG) * d
  when "j" then pa += TURN
  when "l" then pa -= TURN
  end
  pa -= 360.0 while pa >= 360.0
  pa += 360.0 while pa < 0.0

  renderer.set_player(px.to_i, py.to_i, pz.to_i, pa.to_i)
  renderer.render_frame
  $stdout.write(renderer.framebuffer.pack("C*"))
  $stdout.flush
end
