#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class DatavideoViscaFramer {
public:
	static std::vector<uint8_t> encode(const uint8_t *data, size_t size)
	{
		if (!data || !size || size > UINT16_MAX - 2)
			return {};
		const uint16_t frame_size = static_cast<uint16_t>(size + 2);
		std::vector<uint8_t> frame{static_cast<uint8_t>(frame_size >> 8), static_cast<uint8_t>(frame_size)};
		frame.insert(frame.end(), data, data + size);
		return frame;
	}

	bool feed(const uint8_t *data, size_t size, std::vector<std::vector<uint8_t>> &frames)
	{
		if (data && size)
			buffer.insert(buffer.end(), data, data + size);

		while (buffer.size() >= 2) {
			const size_t frame_size = (static_cast<size_t>(buffer[0]) << 8) | buffer[1];
			if (frame_size < 3) {
				buffer.clear();
				return false;
			}
			if (buffer.size() < frame_size)
				return true;
			frames.emplace_back(buffer.begin() + 2, buffer.begin() + frame_size);
			buffer.erase(buffer.begin(), buffer.begin() + frame_size);
		}
		return true;
	}

	void clear() { buffer.clear(); }

private:
	std::vector<uint8_t> buffer;
};
