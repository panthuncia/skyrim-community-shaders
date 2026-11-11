// Scaleform GFx 3.3's fill vertex shaders (GRendererD3D1x, the exe's VS-A table), transcribed from the HLSL source the exe
// embeds beside the bytecode (docs/development/p5/scaleform.md, "Shaders"). FXC builds each variant as the game's own: the
// same signature and constants, and the same instructions as the embedded source compiled by FXC.
//
// VS_KIND, the D3D11 renderer's vertex shader index:
//   1 XY16i, one texgen (the solid, bitmap and line draws)   2 glyph: XY32f, TEXCOORD0, COLOR0 (DrawBitmaps)
//   3 XY16iC32, one texgen                                  4 XY16iCF32, one texgen   5 XY16iCF32, two texgens
// VS_3D: the 3D twin of the kind (the renderer's index + 6): mvp is the full float4x4.
//
// The ORG build (CS_ORG_BINDLESS) has no input layout or constant buffer: it reads the vertex and the constants from the
// display's staging buffer (Scaleform/Fill.hlsli) and computes the same outputs.

#ifndef VS_KIND
#	define VS_KIND 1
#endif

#if defined(CS_ORG_BINDLESS)

#	include "Scaleform/Fill.hlsli"

ScaleformFillVaryings main(uint vertexID : SV_VertexID)
{
	const uint address = ScaleformVertexAddress(vertexID);
#	if VS_KIND == 2
	const float4 pos = ScaleformFloat2(address);
#	else
	const float4 pos = ScaleformPositionXY16i(address);
#	endif

	ScaleformFillVaryings output;
	output.color = 0.0f;
	output.factor = 0.0f;
	output.tc0 = 0.0f;
	output.tc1 = 0.0f;
	output.position = pos;
#	if defined(VS_3D)
	// mul(pos, mvp) of a column-major float4x4: each memory row is a column.
	output.position = float4(dot(pos, FillRecord.mvp[0]), dot(pos, FillRecord.mvp[1]), dot(pos, FillRecord.mvp[2]), dot(pos, FillRecord.mvp[3]));
#	else
	output.position.x = dot(pos, FillRecord.mvp[0]);
	output.position.y = dot(pos, FillRecord.mvp[1]);
#	endif
#	if VS_KIND == 2
	output.tc0 = ScaleformFloat2(address + 8).xy;
	output.color = ScaleformColorR8G8B8A8(address + 16).bgra;
#	else
	output.tc0.x = dot(pos, FillRecord.texgen[0]);
	output.tc0.y = dot(pos, FillRecord.texgen[1]);
#	endif
#	if VS_KIND >= 3
	output.color = ScaleformColorR8G8B8A8(address + 4);
#	endif
#	if VS_KIND >= 4
	output.factor = ScaleformColorR8G8B8A8(address + 8);
#	endif
#	if VS_KIND == 5
	output.tc1.x = dot(pos, FillRecord.texgen[2]);
	output.tc1.y = dot(pos, FillRecord.texgen[3]);
#	endif
	return output;
}

#elif !defined(VS_3D)

#	if VS_KIND == 1
cbuffer VSConstants
{
	float4 mvp[4];
	float4 texgen[2];
}
void main(int4 pos : POSITION,
	out float2 otc0 : TEXCOORD0,
	out float4 opos : SV_Position)
{
	opos = pos;
	opos.x = dot(pos, mvp[0]);
	opos.y = dot(pos, mvp[1]);
	otc0.x = dot(pos, texgen[0]);
	otc0.y = dot(pos, texgen[1]);
}
#	elif VS_KIND == 2
cbuffer VSConstants
{
	float4 mvp[4];
}
void main(float4 pos : POSITION,
	float2 tc0 : TEXCOORD0,
	float4 color : COLOR0,
	out float4 ocolor : COLOR0,
	out float2 otc0 : TEXCOORD0,
	out float4 opos : SV_Position)
{
	opos = pos;
	opos.x = dot(pos, mvp[0]);
	opos.y = dot(pos, mvp[1]);
	otc0 = tc0;
	ocolor = color.bgra;
}
#	elif VS_KIND == 3
cbuffer VSConstants
{
	float4 mvp[4];
	float4 texgen[2];
}
void main(int4 pos : POSITION,
	float4 color : COLOR,
	out float4 ocolor : COLOR,
	out float2 otc0 : TEXCOORD0,
	out float4 opos : SV_Position)
{
	opos = pos;
	opos.x = dot(pos, mvp[0]);
	opos.y = dot(pos, mvp[1]);
	otc0.x = dot(pos, texgen[0]);
	otc0.y = dot(pos, texgen[1]);
	ocolor = color;
}
#	elif VS_KIND == 4
cbuffer VSConstants
{
	float4 mvp[4];
	float4 texgen[2];
}
void main(int4 pos : POSITION,
	float4 color : COLOR,
	float4 factor : COLOR1,
	out float4 ocolor : COLOR,
	out float4 ofactor : COLOR1,
	out float2 otc0 : TEXCOORD0,
	out float4 opos : SV_Position)
{
	opos = pos;
	opos.x = dot(pos, mvp[0]);
	opos.y = dot(pos, mvp[1]);
	otc0.x = dot(pos, texgen[0]);
	otc0.y = dot(pos, texgen[1]);
	ocolor = color;
	ofactor = factor;
}
#	elif VS_KIND == 5
cbuffer VSConstants
{
	float4 mvp[4];
	float4 texgen[4];
}
void main(int4 pos : POSITION,
	float4 color : COLOR,
	float4 factor : COLOR1,
	out float4 ocolor : COLOR,
	out float4 ofactor : COLOR1,
	out float2 otc0 : TEXCOORD0,
	out float2 otc1 : TEXCOORD1,
	out float4 opos : SV_Position)
{
	opos = pos;
	opos.x = dot(pos, mvp[0]);
	opos.y = dot(pos, mvp[1]);
	otc0.x = dot(pos, texgen[0]);
	otc0.y = dot(pos, texgen[1]);
	otc1.x = dot(pos, texgen[2]);
	otc1.y = dot(pos, texgen[3]);
	ocolor = color;
	ofactor = factor;
}
#	endif

#else  // VS_3D

#	if VS_KIND == 1
cbuffer VSConstants
{
	float4x4 mvp;
	float4 texgen[2];
}
void main(int4 pos : POSITION,
	out float2 otc0 : TEXCOORD0,
	out float4 opos : SV_Position)
{
	opos = mul(float4(pos), mvp);
	otc0.x = dot(pos, texgen[0]);
	otc0.y = dot(pos, texgen[1]);
}
#	elif VS_KIND == 2
cbuffer VSConstants
{
	float4x4 mvp;
}
void main(float4 pos : POSITION,
	float2 tc0 : TEXCOORD0,
	float4 color : COLOR0,
	out float4 ocolor : COLOR0,
	out float2 otc0 : TEXCOORD0,
	out float4 opos : SV_Position)
{
	opos = mul(float4(pos), mvp);
	otc0 = tc0;
	ocolor = color.bgra;
}
#	elif VS_KIND == 3
cbuffer VSConstants
{
	float4x4 mvp;
	float4 texgen[2];
}
void main(int4 pos : POSITION,
	float4 color : COLOR,
	out float4 ocolor : COLOR,
	out float2 otc0 : TEXCOORD0,
	out float4 opos : SV_Position)
{
	opos = mul(float4(pos), mvp);
	otc0.x = dot(pos, texgen[0]);
	otc0.y = dot(pos, texgen[1]);
	ocolor = color;
}
#	elif VS_KIND == 4
cbuffer VSConstants
{
	float4x4 mvp;
	float4 texgen[2];
}
void main(int4 pos : POSITION,
	float4 color : COLOR,
	float4 factor : COLOR1,
	out float4 ocolor : COLOR,
	out float4 ofactor : COLOR1,
	out float2 otc0 : TEXCOORD0,
	out float4 opos : SV_Position)
{
	opos = mul(float4(pos), mvp);
	otc0.x = dot(pos, texgen[0]);
	otc0.y = dot(pos, texgen[1]);
	ocolor = color;
	ofactor = factor;
}
#	elif VS_KIND == 5
cbuffer VSConstants
{
	float4x4 mvp;
	float4 texgen[4];
}
void main(int4 pos : POSITION,
	float4 color : COLOR,
	float4 factor : COLOR1,
	out float4 ocolor : COLOR,
	out float4 ofactor : COLOR1,
	out float2 otc0 : TEXCOORD0,
	out float2 otc1 : TEXCOORD1,
	out float4 opos : SV_Position)
{
	opos = mul(float4(pos), mvp);
	otc0.x = dot(pos, texgen[0]);
	otc0.y = dot(pos, texgen[1]);
	otc1.x = dot(pos, texgen[2]);
	otc1.y = dot(pos, texgen[3]);
	ocolor = color;
	ofactor = factor;
}
#	endif

#endif
