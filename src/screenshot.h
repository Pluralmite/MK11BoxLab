// MK11BoxLab - back-buffer screenshots (HDR-safe)
#pragma once
#include <string>
struct IDXGISwapChain;
struct ID3D11Device;
struct ID3D11DeviceContext;

namespace shot {
void Request(const std::string& dir = std::string(), bool keepMenus = false);   // take one on the next frames (menus hidden); dir overrides Pictures\\MK11 Box Lab
bool HidingMenus();             // true while a capture is pending: don't draw the menus
void OnPresent(IDXGISwapChain* sc, ID3D11Device* dev, ID3D11DeviceContext* ctx);   // after the overlay is drawn
std::string Status();
std::string LastPath();          // full path of the last saved screenshot           // short message for a few seconds after a capture ("" otherwise)
}
