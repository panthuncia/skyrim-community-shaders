#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

/**
* @brief What a declared texture's version holds before its first writer runs (cleanup plan B5).
*
* The revision lane applies it to every version: the first one too, and exported ones. A version is ready only once its birth
* is done, so no frame ever binds one that is not. Nothing in the frame clears or fills a version for its first contents.
* - Undefined: nothing. The first writer writes the whole texture. The only birth a depth or multisampled texture may have.
* - Zero: every texel zero.
* - Value: every texel one value, given as floats and encoded in the texture's format.
* - Fill: every subresource written by a function on the lane, from CPU data.
*
* This header includes nothing of ORG or OMS.
*/
namespace RenderGraph::Frame
{
	/** @brief One subresource a Birth::Fill function writes: rows of texels, one depth slice after another. */
	struct BirthTexels
	{
		std::uint32_t mip = 0;
		std::uint32_t slice = 0;  // the array slice
		std::uint32_t width = 0, height = 0, depth = 1;
		std::uint32_t rowPitch = 0;  // bytes from one row to the next in texels
		std::span<std::byte> texels;  // depth * height rows, each rowPitch bytes apart
	};

	class Birth
	{
	public:
		enum class Kind : std::uint8_t
		{
			Unset,  // not given: History is born Zero, everything else Undefined
			Undefined,
			Zero,
			Value,
			Fill,
		};

		/** @brief Any thread (the revision lane). Writes one subresource. False is fatal. */
		using FillFn = std::function<bool(const BirthTexels&)>;

		static Birth Undefined() { return Birth(Kind::Undefined); }
		static Birth Zero() { return Birth(Kind::Zero); }
		static Birth Value(std::array<float, 4> a_value)
		{
			Birth birth(Kind::Value);
			birth.value = a_value;
			return birth;
		}
		static Birth Fill(FillFn a_fill)
		{
			Birth birth(Kind::Fill);
			birth.fill = std::make_shared<const FillFn>(std::move(a_fill));
			return birth;
		}

		Birth() = default;

		Kind GetKind() const { return kind; }
		const std::array<float, 4>& GetValue() const { return value; }
		const std::shared_ptr<const FillFn>& GetFill() const { return fill; }

		/**
		* @brief The birth when none was given: Zero for a History texture the lane can write (a_writable: single-sampled colour),
		* else Undefined.
		*/
		Birth Resolved(bool a_history, bool a_writable) const
		{
			if (kind != Kind::Unset)
				return *this;
			return a_history && a_writable ? Zero() : Undefined();
		}

		/** @brief Whether the lane writes the version: Zero, Value or Fill. */
		bool Writes() const { return kind == Kind::Zero || kind == Kind::Value || kind == Kind::Fill; }

	private:
		explicit Birth(Kind a_kind) :
			kind(a_kind) {}

		Kind kind = Kind::Unset;
		std::array<float, 4> value{};
		std::shared_ptr<const FillFn> fill;
	};

	/**
	* @brief Encodes a_value as one texel of a_format (a DXGI_FORMAT) into a_out. Returns the texel's size in bytes, or 0 when the
	* format has no encoding here (block-compressed, packed depth, video formats). Any thread.
	*/
	std::uint32_t EncodeTexel(std::uint32_t a_format, const std::array<float, 4>& a_value, std::array<std::byte, 16>& a_out);
}
