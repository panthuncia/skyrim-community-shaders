#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <utility>

#include <Render/Runtime/StreamingUploadTypes.h>

/**
* @brief What a declared texture's version holds before its first writer runs (cleanup plan B5).
*
* The revision lane applies it to every version: the first one too, and exported ones. A version is ready only once its birth
* is done, so no frame ever binds one that is not. Nothing in the frame clears or fills a version for its first contents.
* - Undefined: nothing. The first writer writes the whole texture. The only birth a depth or multisampled texture may have.
* - Zero: every texel zero.
* - Value: every texel one value, given as floats and encoded in the texture's format (OMS's EncodeTexel).
* - Fill: every subresource written by an org::TextureFillWriter on the lane, from CPU data.
*
* Of ORG this header includes only its fill writer type.
*/
namespace RenderGraph::Frame
{
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

		static Birth Undefined() { return Birth(Kind::Undefined); }
		static Birth Zero() { return Birth(Kind::Zero); }
		static Birth Value(std::array<float, 4> a_value)
		{
			Birth birth(Kind::Value);
			birth.value = a_value;
			return birth;
		}
		/**
		* @brief a_fill runs on any thread (the revision lane) and writes one subresource: depth slices of rows texels, each row
		* rowPitch bytes apart. False is fatal.
		*/
		static Birth Fill(org::TextureFillWriter a_fill)
		{
			Birth birth(Kind::Fill);
			birth.fill = std::make_shared<const org::TextureFillWriter>(std::move(a_fill));
			return birth;
		}

		Birth() = default;

		Kind GetKind() const { return kind; }
		const std::array<float, 4>& GetValue() const { return value; }
		const std::shared_ptr<const org::TextureFillWriter>& GetFill() const { return fill; }

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
		std::shared_ptr<const org::TextureFillWriter> fill;
	};
}
