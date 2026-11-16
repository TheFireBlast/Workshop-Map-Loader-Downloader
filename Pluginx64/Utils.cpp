#include "pch.h"
#include <zlib.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")

namespace Utils
{
	std::wstring StringToWideString(const std::string& p_value)
	{
		const int length = MultiByteToWideChar(CP_ACP, 0, p_value.c_str(), static_cast<int>(p_value.length()) + 1, nullptr, 0);
		std::vector<wchar_t> buffer(length);
		MultiByteToWideChar(CP_ACP, 0, p_value.c_str(), static_cast<int>(p_value.length()) + 1, buffer.data(), length);
		return std::wstring(buffer.data());
	}

	std::string ConvertToMB(const std::string& p_value)
	{
		unsigned long long bytes = 0;
		try
		{
			bytes = std::stoull(p_value);
		}
		catch(const std::exception&)
		{
			return "0 Bytes";
		}

		const char* units[] = { "Bytes", "KB", "MB", "GB", "TB" };
		double value = static_cast<double>(bytes);
		int unitIndex = 0;
		while(value >= 1024.0 && unitIndex < 4)
		{
			value /= 1024.0;
			unitIndex++;
		}

		std::ostringstream output;
		if(unitIndex == 0)
			output << bytes;
		else
			output << std::fixed << std::setprecision(value >= 100.0 ? 0 : 1) << value;
		output << " " << units[unitIndex];
		return output.str();
	}

	bool DirectoryOrFileExists(const std::filesystem::path& p_path, std::filesystem::file_status p_status)
	{
		std::error_code error;
		if(p_status.type() == std::filesystem::file_type::none)
			p_status = std::filesystem::status(p_path, error);

		return !error && std::filesystem::exists(p_status);
	}

	// ---- Built-in ZIP extraction (zlib) ----
	// Replaces shelling out to PowerShell / cmd + VBScript, which flashes a console window and does not work
	// under Wine/Proton (no PowerShell, no Shell.Application zip folders).
	// Supports stored and deflated entries, ZIP64 sizes/offsets, and rejects entries that would escape p_destination.
	namespace
	{
		uint16_t ReadU16(const unsigned char* p_data) { return static_cast<uint16_t>(p_data[0] | (p_data[1] << 8)); }
		uint32_t ReadU32(const unsigned char* p_data) { return static_cast<uint32_t>(ReadU16(p_data)) | (static_cast<uint32_t>(ReadU16(p_data + 2)) << 16); }
		uint64_t ReadU64(const unsigned char* p_data) { return static_cast<uint64_t>(ReadU32(p_data)) | (static_cast<uint64_t>(ReadU32(p_data + 4)) << 32); }

		struct ZipEntry
		{
			std::string name;
			uint16_t method = 0;
			uint64_t compressedSize = 0;
			uint64_t uncompressedSize = 0;
			uint64_t localHeaderOffset = 0;
			bool utf8 = false;
		};

		bool ReadAt(std::ifstream& p_file, uint64_t p_offset, void* p_buffer, size_t p_size)
		{
			p_file.clear();
			p_file.seekg(static_cast<std::streamoff>(p_offset), std::ios::beg);
			p_file.read(static_cast<char*>(p_buffer), static_cast<std::streamsize>(p_size));
			return static_cast<size_t>(p_file.gcount()) == p_size;
		}

		std::filesystem::path SafeEntryPath(const std::filesystem::path& p_destination, const ZipEntry& p_entry)
		{
			std::string name = p_entry.name;
			std::replace(name.begin(), name.end(), '\\', '/');
			std::filesystem::path relative = p_entry.utf8 ? std::filesystem::u8path(name) : std::filesystem::path(name);
			relative = relative.lexically_normal();
			if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
				return {};
			for (const auto& part : relative)
				if (part == "..")
					return {};
			return p_destination / relative;
		}
	}

	bool ExtractZip(const std::filesystem::path& p_zipPath, const std::filesystem::path& p_destination, std::string& p_error)
	{
		std::ifstream file(p_zipPath, std::ios::binary);
		if (!file)
		{
			p_error = "cannot open " + p_zipPath.string();
			return false;
		}
		file.seekg(0, std::ios::end);
		const uint64_t fileSize = static_cast<uint64_t>(file.tellg());

		// Find the End Of Central Directory record (last 64 KiB + 22 bytes)
		const uint64_t tailSize = std::min<uint64_t>(fileSize, 0xFFFF + 22);
		std::vector<unsigned char> tail(static_cast<size_t>(tailSize));
		if (!ReadAt(file, fileSize - tailSize, tail.data(), tail.size()))
		{
			p_error = "cannot read the end of the archive";
			return false;
		}
		int64_t eocd = -1;
		for (int64_t i = static_cast<int64_t>(tail.size()) - 22; i >= 0; i--)
			if (ReadU32(&tail[static_cast<size_t>(i)]) == 0x06054b50) { eocd = i; break; }
		if (eocd < 0)
		{
			p_error = "not a ZIP archive (no end of central directory)";
			return false;
		}

		uint64_t entryCount = ReadU16(&tail[static_cast<size_t>(eocd) + 10]);
		uint64_t directorySize = ReadU32(&tail[static_cast<size_t>(eocd) + 12]);
		uint64_t directoryOffset = ReadU32(&tail[static_cast<size_t>(eocd) + 16]);

		// ZIP64 end of central directory locator sits right before the EOCD
		if (eocd >= 20 && ReadU32(&tail[static_cast<size_t>(eocd) - 20]) == 0x07064b50)
		{
			unsigned char zip64[56];
			const uint64_t zip64Offset = ReadU64(&tail[static_cast<size_t>(eocd) - 20 + 8]);
			if (ReadAt(file, zip64Offset, zip64, sizeof(zip64)) && ReadU32(zip64) == 0x06064b50)
			{
				entryCount = ReadU64(zip64 + 32);
				directorySize = ReadU64(zip64 + 40);
				directoryOffset = ReadU64(zip64 + 48);
			}
		}

		if (directoryOffset + directorySize > fileSize)
		{
			p_error = "central directory is out of range";
			return false;
		}
		std::vector<unsigned char> directory(static_cast<size_t>(directorySize));
		if (!ReadAt(file, directoryOffset, directory.data(), directory.size()))
		{
			p_error = "cannot read the central directory";
			return false;
		}

		std::vector<ZipEntry> entries;
		size_t position = 0;
		for (uint64_t index = 0; index < entryCount; index++)
		{
			if (position + 46 > directory.size() || ReadU32(&directory[position]) != 0x02014b50)
			{
				p_error = "corrupt central directory";
				return false;
			}
			const unsigned char* header = &directory[position];
			ZipEntry entry;
			entry.utf8 = (ReadU16(header + 8) & 0x0800) != 0;
			entry.method = ReadU16(header + 10);
			entry.compressedSize = ReadU32(header + 20);
			entry.uncompressedSize = ReadU32(header + 24);
			const uint16_t nameLength = ReadU16(header + 28);
			const uint16_t extraLength = ReadU16(header + 30);
			const uint16_t commentLength = ReadU16(header + 32);
			entry.localHeaderOffset = ReadU32(header + 42);
			if (position + 46 + nameLength + extraLength + commentLength > directory.size())
			{
				p_error = "corrupt central directory entry";
				return false;
			}
			entry.name.assign(reinterpret_cast<const char*>(header + 46), nameLength);

			// ZIP64 extended information: only the fields saturated in the header are present, in this order
			const unsigned char* extra = header + 46 + nameLength;
			for (size_t e = 0; e + 4 <= extraLength;)
			{
				const uint16_t id = ReadU16(extra + e);
				const uint16_t size = ReadU16(extra + e + 2);
				if (id == 0x0001)
				{
					size_t field = e + 4;
					if (entry.uncompressedSize == 0xFFFFFFFF && field + 8 <= e + 4 + size) { entry.uncompressedSize = ReadU64(extra + field); field += 8; }
					if (entry.compressedSize == 0xFFFFFFFF && field + 8 <= e + 4 + size) { entry.compressedSize = ReadU64(extra + field); field += 8; }
					if (entry.localHeaderOffset == 0xFFFFFFFF && field + 8 <= e + 4 + size) { entry.localHeaderOffset = ReadU64(extra + field); }
				}
				e += 4 + size;
			}
			entries.push_back(entry);
			position += 46 + nameLength + extraLength + commentLength;
		}

		std::error_code error;
		std::filesystem::create_directories(p_destination, error);
		std::vector<unsigned char> input(1 << 16), output(1 << 16);
		for (const ZipEntry& entry : entries)
		{
			const std::filesystem::path target = SafeEntryPath(p_destination, entry);
			if (target.empty())
			{
				p_error = "unsafe path in archive: " + entry.name;
				return false;
			}
			if (!entry.name.empty() && (entry.name.back() == '/' || entry.name.back() == '\\'))
			{
				std::filesystem::create_directories(target, error);
				continue;
			}
			if (entry.method != 0 && entry.method != 8)
			{
				p_error = "unsupported compression method " + std::to_string(entry.method) + " for " + entry.name;
				return false;
			}

			unsigned char local[30];
			if (!ReadAt(file, entry.localHeaderOffset, local, sizeof(local)) || ReadU32(local) != 0x04034b50)
			{
				p_error = "corrupt local header for " + entry.name;
				return false;
			}
			uint64_t dataOffset = entry.localHeaderOffset + 30 + ReadU16(local + 26) + ReadU16(local + 28);
			if (dataOffset + entry.compressedSize > fileSize)
			{
				p_error = "entry data out of range: " + entry.name;
				return false;
			}

			std::filesystem::create_directories(target.parent_path(), error);
			std::ofstream out(target, std::ios::binary | std::ios::trunc);
			if (!out)
			{
				p_error = "cannot create " + target.string();
				return false;
			}

			file.clear();
			file.seekg(static_cast<std::streamoff>(dataOffset), std::ios::beg);
			uint64_t remaining = entry.compressedSize;
			uint64_t written = 0;

			if (entry.method == 0)
			{
				while (remaining > 0)
				{
					const size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, input.size()));
					file.read(reinterpret_cast<char*>(input.data()), chunk);
					if (static_cast<size_t>(file.gcount()) != chunk)
					{
						p_error = "truncated data for " + entry.name;
						return false;
					}
					out.write(reinterpret_cast<const char*>(input.data()), chunk);
					remaining -= chunk;
					written += chunk;
				}
			}
			else
			{
				z_stream stream = {};
				if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
				{
					p_error = "zlib init failed";
					return false;
				}
				int status = Z_OK;
				while (status != Z_STREAM_END)
				{
					if (stream.avail_in == 0)
					{
						if (remaining == 0)
							break;
						const size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, input.size()));
						file.read(reinterpret_cast<char*>(input.data()), chunk);
						if (static_cast<size_t>(file.gcount()) != chunk)
							break;
						remaining -= chunk;
						stream.next_in = input.data();
						stream.avail_in = static_cast<uInt>(chunk);
					}
					stream.next_out = output.data();
					stream.avail_out = static_cast<uInt>(output.size());
					status = inflate(&stream, Z_NO_FLUSH);
					if (status != Z_OK && status != Z_STREAM_END)
						break;
					const size_t produced = output.size() - stream.avail_out;
					out.write(reinterpret_cast<const char*>(output.data()), produced);
					written += produced;
				}
				inflateEnd(&stream);
				if (status != Z_STREAM_END)
				{
					p_error = "corrupt deflate data for " + entry.name;
					return false;
				}
			}

			out.close();
			if (!out || written != entry.uncompressedSize)
			{
				p_error = "size mismatch or write failure for " + entry.name;
				return false;
			}
		}
		return true;
	}

	void CreateUnzipBatchFile(const std::filesystem::path& p_bakkesmodPath, const std::string& p_destinationPath, const std::string& p_zipFilePath)
	{
		const std::filesystem::path batchPath = p_bakkesmodPath / "data" / "WorkshopMapLoader" / "temp" / "unzip.bat";
		std::filesystem::create_directories(batchPath.parent_path());

		std::string extractError;
		if (ExtractZip(std::filesystem::path(p_zipFilePath), std::filesystem::path(p_destinationPath), extractError))
			return;
		if (_globalCvarManager)
			_globalCvarManager->log("Built-in unzip failed (" + extractError + "), falling back to the batch script");

		std::ofstream batchFile(batchPath);
		std::string destinationPath = p_destinationPath;
		std::string zipFilePath = p_zipFilePath;
		std::replace(destinationPath.begin(), destinationPath.end(), '/', '\\');
		std::replace(zipFilePath.begin(), zipFilePath.end(), '/', '\\');

		batchFile << "@echo off\nsetlocal\ncd /d %~dp0\n";
		batchFile << "Call :UnZipFile \"" + destinationPath + "\" \"" + zipFilePath + "\"\nexit /b\n\n";
		batchFile << ":UnZipFile <ExtractTo> <newzipfile>\n";
		batchFile << "set vbs=\"%temp%\\_.vbs\"\nif exist %vbs% del /f /q %vbs%\n";
		batchFile << ">%vbs% echo Set fso = CreateObject(\"Scripting.FileSystemObject\")\n";
		batchFile << ">>%vbs% echo If NOT fso.FolderExists(%1) Then\n>>%vbs% echo fso.CreateFolder(%1)\n>>%vbs% echo End If\n";
		batchFile << ">>%vbs% echo set objShell = CreateObject(\"Shell.Application\")\n";
		batchFile << ">>%vbs% echo set FilesInZip=objShell.NameSpace(%2).items\n";
		batchFile << ">>%vbs% echo objShell.NameSpace(%1).CopyHere(FilesInZip)\n";
		batchFile << ">>%vbs% echo Set fso = Nothing\n>>%vbs% echo Set objShell = Nothing\n";
		batchFile << "cscript //nologo %vbs%\nif exist %vbs% del /f /q %vbs%\n";
		batchFile.close();
		system(batchPath.string().c_str());
	}

	namespace
	{
		template <typename T> void ReleaseCom(T*& p_object) { if (p_object) { p_object->Release(); p_object = nullptr; } }

		// Decodes any WIC-supported image to 32bpp BGRA
		bool DecodeImageBGRA(const std::filesystem::path& p_path, std::vector<unsigned char>& p_pixels, UINT& p_width, UINT& p_height)
		{
			IWICImagingFactory* factory = nullptr;
			IWICBitmapDecoder* decoder = nullptr;
			IWICBitmapFrameDecode* frame = nullptr;
			IWICFormatConverter* converter = nullptr;
			bool ok = false;
			if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
				SUCCEEDED(factory->CreateDecoderFromFilename(p_path.wstring().c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
				SUCCEEDED(decoder->GetFrame(0, &frame)) &&
				SUCCEEDED(frame->GetSize(&p_width, &p_height)) && p_width > 0 && p_height > 0 &&
				SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
				SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
			{
				p_pixels.resize(static_cast<size_t>(p_width) * p_height * 4);
				ok = SUCCEEDED(converter->CopyPixels(nullptr, p_width * 4, static_cast<UINT>(p_pixels.size()), p_pixels.data()));
			}
			ReleaseCom(converter);
			ReleaseCom(frame);
			ReleaseCom(decoder);
			ReleaseCom(factory);
			return ok;
		}

		// Area-averaging downscale (each destination pixel is the mean of the source pixels it covers)
		std::vector<unsigned char> BoxDownscaleBGRA(const std::vector<unsigned char>& p_source, UINT p_sourceWidth, UINT p_sourceHeight,
			UINT p_width, UINT p_height)
		{
			std::vector<unsigned char> result(static_cast<size_t>(p_width) * p_height * 4);
			for (UINT y = 0; y < p_height; y++)
			{
				const UINT y0 = static_cast<UINT>(static_cast<unsigned long long>(y) * p_sourceHeight / p_height);
				UINT y1 = static_cast<UINT>(static_cast<unsigned long long>(y + 1) * p_sourceHeight / p_height);
				if (y1 <= y0) y1 = y0 + 1;
				for (UINT x = 0; x < p_width; x++)
				{
					const UINT x0 = static_cast<UINT>(static_cast<unsigned long long>(x) * p_sourceWidth / p_width);
					UINT x1 = static_cast<UINT>(static_cast<unsigned long long>(x + 1) * p_sourceWidth / p_width);
					if (x1 <= x0) x1 = x0 + 1;
					unsigned long long sum[4] = {};
					for (UINT sy = y0; sy < y1; sy++)
					{
						const unsigned char* row = &p_source[(static_cast<size_t>(sy) * p_sourceWidth + x0) * 4];
						for (UINT sx = x0; sx < x1; sx++, row += 4)
							for (int c = 0; c < 4; c++)
								sum[c] += row[c];
					}
					const unsigned long long count = static_cast<unsigned long long>(y1 - y0) * (x1 - x0);
					unsigned char* out = &result[(static_cast<size_t>(y) * p_width + x) * 4];
					for (int c = 0; c < 4; c++)
						out[c] = static_cast<unsigned char>((sum[c] + count / 2) / count);
				}
			}
			return result;
		}

		// Writes a 24-bit bottom-up BMP (no alpha needed for previews; every WIC build reads it)
		bool WriteBMP24(const std::filesystem::path& p_path, const std::vector<unsigned char>& p_bgra, UINT p_width, UINT p_height)
		{
			const UINT rowSize = (p_width * 3 + 3) & ~3u;
			const UINT imageSize = rowSize * p_height;
			unsigned char header[54] = { 'B', 'M' };
			auto put32 = [&](int p_offset, UINT p_value) { for (int i = 0; i < 4; i++) header[p_offset + i] = static_cast<unsigned char>(p_value >> (8 * i)); };
			put32(2, 54 + imageSize);
			put32(10, 54);
			put32(14, 40);
			put32(18, p_width);
			put32(22, p_height);
			header[26] = 1;
			header[28] = 24;
			put32(34, imageSize);
			std::ofstream file(p_path, std::ios::binary | std::ios::trunc);
			if (!file)
				return false;
			file.write(reinterpret_cast<const char*>(header), sizeof(header));
			std::vector<unsigned char> row(rowSize, 0);
			for (UINT y = 0; y < p_height; y++)
			{
				const unsigned char* source = &p_bgra[static_cast<size_t>(p_height - 1 - y) * p_width * 4];
				for (UINT x = 0; x < p_width; x++)
				{
					row[x * 3 + 0] = source[x * 4 + 0];
					row[x * 3 + 1] = source[x * 4 + 1];
					row[x * 3 + 2] = source[x * 4 + 2];
				}
				file.write(reinterpret_cast<const char*>(row.data()), rowSize);
			}
			return static_cast<bool>(file);
		}
	}

	std::filesystem::path GetThumbnail(const std::filesystem::path& p_image, const std::filesystem::path& p_cacheDirectory,
		int p_coverWidth, int p_coverHeight)
	{
		std::error_code error;
		const auto fileSize = std::filesystem::file_size(p_image, error);
		if (error)
			return p_image;
		const auto modified = std::filesystem::last_write_time(p_image, error).time_since_epoch().count();
		if (error)
			return p_image;

		const std::string key = std::to_string(std::hash<std::wstring>{}(p_image.wstring())) + "_" +
			std::to_string(fileSize) + "_" + std::to_string(modified) + "_" + std::to_string(p_coverWidth) + "x" + std::to_string(p_coverHeight);
		const std::filesystem::path thumbnail = p_cacheDirectory / (key + ".bmp");
		if (std::filesystem::exists(thumbnail, error))
			return thumbnail;

		std::vector<unsigned char> pixels;
		UINT width = 0, height = 0;
		if (!DecodeImageBGRA(p_image, pixels, width, height))
			return p_image;

		// Scale so the image covers the target box; keep the whole image (drawing code crops by UV)
		const double scale = (std::max)(static_cast<double>(p_coverWidth) / width, static_cast<double>(p_coverHeight) / height);
		if (scale >= 0.75) // already close to display size: bilinear sampling is fine
			return p_image;
		const UINT newWidth = (std::max)(1u, static_cast<UINT>(width * scale + 0.5));
		const UINT newHeight = (std::max)(1u, static_cast<UINT>(height * scale + 0.5));

		std::filesystem::create_directories(p_cacheDirectory, error);
		const std::filesystem::path temporary = p_cacheDirectory / (key + ".tmp");
		if (!WriteBMP24(temporary, BoxDownscaleBGRA(pixels, width, height, newWidth, newHeight), newWidth, newHeight))
			return p_image;
		std::filesystem::rename(temporary, thumbnail, error);
		return error ? p_image : thumbnail;
	}

	void AddImageCover(ImDrawList* p_drawList, ImageWrapper& p_image, const ImVec2& p_min, const ImVec2& p_max)
	{
		ImTextureID texture = p_image.GetImGuiTex();
		if (!texture)
			return;

		ImVec2 uv0(0.f, 0.f), uv1(1.f, 1.f);
		const Vector2 imageSize = p_image.GetSize();
		const float boxWidth = p_max.x - p_min.x;
		const float boxHeight = p_max.y - p_min.y;
		if (imageSize.X > 0 && imageSize.Y > 0 && boxWidth > 0.f && boxHeight > 0.f)
		{
			const float imageAspect = static_cast<float>(imageSize.X) / static_cast<float>(imageSize.Y);
			const float boxAspect = boxWidth / boxHeight;
			if (imageAspect > boxAspect)
			{
				const float visible = boxAspect / imageAspect; // fraction of the width that fits
				uv0.x = (1.f - visible) * 0.5f;
				uv1.x = 1.f - uv0.x;
			}
			else
			{
				const float visible = imageAspect / boxAspect; // fraction of the height that fits
				uv0.y = (1.f - visible) * 0.5f;
				uv1.y = 1.f - uv0.y;
			}
		}
		p_drawList->AddImage(texture, p_min, p_max, uv0, uv1);
	}

	ImVec2 FitImageSize(ImageWrapper& p_image, const ImVec2& p_box)
	{
		const Vector2 imageSize = p_image.GetSize();
		if (imageSize.X <= 0 || imageSize.Y <= 0 || p_box.x <= 0.f || p_box.y <= 0.f)
			return p_box;
		const float scale = (std::min)(p_box.x / static_cast<float>(imageSize.X), p_box.y / static_cast<float>(imageSize.Y));
		return ImVec2(imageSize.X * scale, imageSize.Y * scale);
	}

	void RenderProgressBar(float p_value, float p_maxValue, ImVec2 p_position, ImVec2 p_size, ImColor p_backgroundColor, ImColor p_progressColor,
		const char* p_label)
	{
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const float maximum = p_maxValue > 0.f ? p_maxValue : 1.f;
		const float percent = std::clamp(p_value / maximum, 0.f, 1.f);
		ImGui::BeginChild("##ProgessBar", p_size);
		drawList->AddRectFilled(p_position, ImVec2(p_position.x + p_size.x, p_position.y + p_size.y), p_backgroundColor, 15.f, 15.f);
		if(p_value > 0.f)
			drawList->AddRectFilled(p_position, ImVec2(p_position.x + percent * p_size.x, p_position.y + p_size.y), p_progressColor, 15.f, 15.f);
		const float textWidth = ImGui::CalcTextSize(p_label).x;
		drawList->AddText(ImVec2(p_position.x + (p_size.x - textWidth) * 0.5f, p_position.y + 5.f), ImColor(255, 255, 255, 255), p_label);
		ImGui::EndChild();
	}

	void RenderYesNoPopup(const char* p_popupName, const char* p_label, const char* p_yesText, const char* p_noText,
		std::function<void()> p_yesFunction, std::function<void()> p_noFunction)
	{
		if(!ImGui::BeginPopupModal(p_popupName, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			return;

		ImGui::Text("%s", p_label);
		ImGui::NewLine();
		if(ImGui::Button(p_yesText, ImVec2(100.f, 25.f)))
			p_yesFunction();
		ImGui::SameLine();
		if(ImGui::Button(p_noText, ImVec2(100.f, 25.f)))
			p_noFunction();
		ImGui::EndPopup();
	}
}
