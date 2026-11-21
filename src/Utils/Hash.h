#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Util
{
	/** @brief Mixes value into signature, boost::hash_combine style. */
	inline void CombineHash(std::size_t& signature, std::size_t value) noexcept
	{
		signature ^= value + 0x9E3779B9u + (signature << 6) + (signature >> 2);
	}

	/**
	 * @brief A 64-bit FNV-1a hash.
	 *
	 * Mix folds in one whole word per step. Bytes folds in one byte per step, the textbook FNV-1a.
	 * The two give different values for the same data, so a hash that is stored or compared keeps the form it used.
	 */
	struct Fnv1a
	{
		static constexpr std::uint64_t kOffset = 0xcbf29ce484222325ull;
		static constexpr std::uint64_t kPrime = 0x100000001b3ull;

		std::uint64_t value = kOffset;

		/** @brief Folds in one word. */
		constexpr Fnv1a& Mix(std::uint64_t a_value) noexcept
		{
			value = (value ^ a_value) * kPrime;
			return *this;
		}

		/** @brief Folds in a pointer's address as one word. */
		Fnv1a& Mix(const void* a_pointer) noexcept
		{
			return Mix(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(a_pointer)));
		}

		/** @brief Folds in a_size bytes, one at a time. */
		Fnv1a& Bytes(const void* a_data, std::size_t a_size) noexcept
		{
			const auto* bytes = static_cast<const unsigned char*>(a_data);
			for (std::size_t i = 0; i < a_size; ++i)
				Mix(static_cast<std::uint64_t>(bytes[i]));
			return *this;
		}

		/** @brief Folds in a string's bytes, one at a time. */
		constexpr Fnv1a& Bytes(std::string_view a_text) noexcept
		{
			for (const char c : a_text)
				Mix(static_cast<std::uint64_t>(static_cast<unsigned char>(c)));
			return *this;
		}

		constexpr std::uint64_t Value() const noexcept { return value; }
	};

	/** @brief The 32-bit FNV-1a hash, for keys and witnesses that are 32 bits wide. Same rules as Fnv1a. */
	struct Fnv1a32
	{
		static constexpr std::uint32_t kOffset = 2166136261u;
		static constexpr std::uint32_t kPrime = 16777619u;

		std::uint32_t value = kOffset;

		constexpr Fnv1a32& Mix(std::uint32_t a_value) noexcept
		{
			value = (value ^ a_value) * kPrime;
			return *this;
		}

		Fnv1a32& Bytes(const void* a_data, std::size_t a_size) noexcept
		{
			const auto* bytes = static_cast<const unsigned char*>(a_data);
			for (std::size_t i = 0; i < a_size; ++i)
				Mix(static_cast<std::uint32_t>(bytes[i]));
			return *this;
		}

		constexpr Fnv1a32& Bytes(std::string_view a_text) noexcept
		{
			for (const char c : a_text)
				Mix(static_cast<std::uint32_t>(static_cast<unsigned char>(c)));
			return *this;
		}

		constexpr std::uint32_t Value() const noexcept { return value; }
	};
}
