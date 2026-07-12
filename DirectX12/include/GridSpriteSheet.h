#pragma once
#include <DirectXMath.h>
#include <ResourceManager.h>

// Static sibling of GridSpriteAnimation -- for pre-made TILESETS (world_tileset.png: bricks,
// ground, water, decorations, all distinct art, nothing meant to cycle) rather than character
// animation sheets. No timer/frame-advance at all: you pick a (col, row) cell once per draw call
// (e.g. from tileID -> a lookup table you define) and get that cell's UV rect back. Same grid-math
// assumption as GridSpriteAnimation -- a UNIFORM grid, every cell the same pixel size.
namespace JLib
{
	class GridSpriteSheet
	{
	public:
		GridSpriteSheet() = default;
		GridSpriteSheet(JLib::TextureHandle sheet, int framesX, int framesY)
			: sheet(sheet), framesX(framesX), framesY(framesY)
		{}

		JLib::TextureHandle GetTexture() const { return sheet; }

		DirectX::XMFLOAT2 GetUVOffset(int col, int row) const
		{
			return { (float)col / (float)framesX, (float)row / (float)framesY };
		}

		DirectX::XMFLOAT2 GetUVScale() const
		{
			return { 1.0f / (float)framesX, 1.0f / (float)framesY };
		}

	private:
		JLib::TextureHandle sheet;
		int framesX = 1, framesY = 1;
	};
};