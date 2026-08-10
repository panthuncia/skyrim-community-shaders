#include <OpenRenderGraph/ContributorAPI.h>
#include <CommunityShaders/RenderGraphHost.h>
#include <rhi.h>

#include <type_traits>

static_assert(static_cast<unsigned>(ORG_RG_FORMAT_UNKNOWN) == static_cast<unsigned>(rhi::Format::Unknown));
static_assert(static_cast<unsigned>(ORG_RG_FORMAT_D32_FLOAT) == static_cast<unsigned>(rhi::Format::D32_Float));
static_assert(static_cast<unsigned>(ORG_RG_FORMAT_BC7_UNORM_SRGB) == static_cast<unsigned>(rhi::Format::BC7_UNorm_sRGB));
static_assert(static_cast<unsigned>(ORG_RG_FORMAT_COUNT) == static_cast<unsigned>(rhi::Format::BC7_UNorm_sRGB) + 1);

int main() { return 0; }
