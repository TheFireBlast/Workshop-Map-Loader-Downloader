#pragma once

namespace Utils
{
	std::wstring StringToWideString(const std::string& p_value);
	std::string ConvertToMB(const std::string& p_value);
	bool DirectoryOrFileExists(const std::filesystem::path& p_path, std::filesystem::file_status p_status = std::filesystem::file_status{});
	// Extracts p_zipPath into p_destination in-process (zlib). Returns false and sets p_error on failure.
	bool ExtractZip(const std::filesystem::path& p_zipPath, const std::filesystem::path& p_destination, std::string& p_error);
	void CreateUnzipBatchFile(const std::filesystem::path& p_bakkesmodPath, const std::string& p_destinationPath, const std::string& p_zipFilePath);
	// Draws p_image filling [p_min, p_max] without stretching: the image is scaled to cover the box and
	// the overflow is cropped equally on both sides.
	void AddImageCover(ImDrawList* p_drawList, ImageWrapper& p_image, const ImVec2& p_min, const ImVec2& p_max);
	// Size p_image scaled to fit inside p_box, keeping its aspect ratio.
	ImVec2 FitImageSize(ImageWrapper& p_image, const ImVec2& p_box);
	void RenderProgressBar(float p_value, float p_maxValue, ImVec2 p_position, ImVec2 p_size, ImColor p_backgroundColor, ImColor p_progressColor,
		const char* p_label);
	void RenderYesNoPopup(const char* p_popupName, const char* p_label, const char* p_yesText, const char* p_noText,
		std::function<void()> p_yesFunction, std::function<void()> p_noFunction);
}
