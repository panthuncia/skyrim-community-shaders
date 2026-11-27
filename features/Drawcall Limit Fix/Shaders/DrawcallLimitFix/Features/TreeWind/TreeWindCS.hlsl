// Drawcall Limit Fix. Tree wind on the GPU.
// The engine's tree manager (FUN_1404381e0) advances each BSTreeNode's clock and gust, and SetupGeometry case 0xc turns the node into
// TreeParams and WindTimers. This pass makes the same from the same inputs, so nothing reads tree nodes per frame.
// One thread per wind entry. Entry 0 is the nodeless trees', entry s + 1 is tree slot s. A row with a new generation starts from the
// node's values. The buffers alternate by frame and the next frame draws this one's, so nothing waits for this pass.

// Must match TreeWindConstants in TreeWind.h.
cbuffer TreeWindConstants : register(b0)
{
	uint TreesIndex;    // StructuredBuffer<TreeStatic>
	uint ClocksIndex;   // RWStructuredBuffer<TreeClock>
	uint FrameIndex;    // StructuredBuffer<TreeWindFrameRow>, one row (every commit's)
	uint Padding0;
	uint2 WindIndices;  // RWStructuredBuffer<float4>, the two wind buffers, by render parity (WindSlot)
	uint2 Padding1;
}

// A buffer, not constants, because the pass is prepared before the commit that knows them. The counts end the dispatch.
struct TreeWindFrameRow
{
	uint TreeCount;    // tree slots
	uint WindSlot;     // the wind buffer this render writes
	uint Frame;
	uint TreeSeedsIndex;  // StructuredBuffer<TreeStatic>, the seeds' pool, two rows a slot
	float DeltaTime;
	float CameraX;
	float CameraY;
	float CameraZ;
	float WindSpeed;
	float MaxDistance2;
	float WindMagnitude;
	float FadeStart;
	float FadeEnd;
	float TimerScale;
	uint RowPadding1;
	uint RowPadding2;
};

static TreeWindFrameRow W;

// Must match Records.h's TreeStatic and TreeWind.h's TreeClock.
struct TreeStatic
{
	float3 Position;
	float LeafFrequency;
	float ModelAmplitude;
	float Timer;
	float PreviousTimer;
	float Amplitude;
	uint Generation;
	uint Animated;
	uint SeedOdd;  // which of the slot's two seed rows holds its generation's
	uint Padding;
};

struct TreeClock
{
	float Timer;
	float PreviousTimer;
	float Amplitude;
	uint Generation;
	uint Frame;
	uint3 Padding;
};

static const uint kNodelessTree = 0xFFFFFFFEu;

// FUN_140438950(x, 3). A quarter of the sum of sin(k pi x) for k = 1, 3, 5, 7, using the engine's argument reduction and odd
// polynomial (constants at 0x141846e64), summed in its order.
float Gust(float a_x)
{
	precise float4 v = a_x * float4(3.14159274f, 9.42477798f, 15.7079639f, 21.9911499f);
	precise float4 turns = v * 0.159154937f;
	v = v - round(turns) * 6.28318548f;
	precise float4 v2 = v * v;
	precise float4 v3 = v2 * v;
	precise float4 s = (-0.166521862f * v3 + v) + 0.00819991343f * (v3 * v2) + -0.000161475939f * (v3 * v2 * v2);
	s = s * 0.25f;
	return (s.w + s.z) + (s.y + s.x);
}

// The engine's square root, 0x5f3759df and one Newton step, as LightingDescriptors.cpp's FastSqrt.
float FastSqrt(float a_value)
{
	const int bits = asint(a_value);
	const float estimate = asfloat(uint(0x5f3759df - (bits >> 1)));
	precise float result = (1.5f - a_value * 0.5f * estimate * estimate) * estimate * a_value;
	return result;
}

float Distance2(float3 a_position)
{
	precise float3 d = a_position - float3(W.CameraX, W.CameraY, W.CameraZ);
	return d.x * d.x + d.y * d.y + d.z * d.z;
}

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	const uint entry = dispatchID.x;
	StructuredBuffer<TreeWindFrameRow> frames = ResourceDescriptorHeap[FrameIndex];
	W = frames[0];
	if (entry > W.TreeCount)
		return;
	// SetupGeometry case 0xc with no node. Distance 0, amplitude 1, leaf frequency 1, no clock.
	float distance2 = 0.0f, amplitude = 1.0f, leafFrequency = 1.0f, timer = 0.0f, previousTimer = 0.0f;
	uint generation = kNodelessTree;
	if (entry != 0) {
		const uint slot = entry - 1;
		StructuredBuffer<TreeStatic> trees = ResourceDescriptorHeap[TreesIndex];
		RWStructuredBuffer<TreeClock> clocks = ResourceDescriptorHeap[ClocksIndex];
		TreeStatic tree = trees[slot];
		// The node's values come from its seed, matching TreeWind.h's MergeTreeSeed. With no seed of this generation, no clock step and no entry.
		{
			if (W.TreeSeedsIndex == 0)
				return;
			StructuredBuffer<TreeStatic> seeds = ResourceDescriptorHeap[W.TreeSeedsIndex];
			uint count, stride;
			seeds.GetDimensions(count, stride);
			const uint at = 2u * slot + (tree.SeedOdd & 1u);
			if (at >= count || seeds[at].Generation != tree.Generation)
				return;
			const uint odd = tree.SeedOdd;
			tree = seeds[at];
			tree.SeedOdd = odd;
		}
		TreeClock clock = clocks[slot];
		if (clock.Generation != tree.Generation) {
			clock.Timer = tree.Timer;
			clock.PreviousTimer = tree.PreviousTimer;
			clock.Amplitude = tree.Amplitude;
			clock.Generation = tree.Generation;
			clock.Frame = W.Frame;
		} else if (clock.Frame != W.Frame) {
			// SetupGeometry kept last frame's timer as the previous one. The manager advances a node with a model, and takes its gust only within range.
			clock.PreviousTimer = clock.Timer;
			if (tree.Animated != 0) {
				clock.Timer = clock.Timer + W.DeltaTime;
				if (Distance2(tree.Position) < W.MaxDistance2)
					clock.Amplitude = Gust(W.WindSpeed * clock.Timer) * tree.ModelAmplitude;
			}
			clock.Frame = W.Frame;
		}
		clocks[slot] = clock;
		distance2 = Distance2(tree.Position);
		amplitude = clock.Amplitude;
		leafFrequency = tree.LeafFrequency;
		timer = clock.Timer;
		previousTimer = clock.PreviousTimer;
		generation = tree.Generation;
	}
	const float distance = entry != 0 ? FastSqrt(distance2) : 0.0f;
	precise float faded = (1.0f - (distance - W.FadeStart) / (W.FadeEnd - W.FadeStart)) * amplitude;
	faded = min(max(faded, 0.0f), amplitude);
	RWStructuredBuffer<float4> wind = ResourceDescriptorHeap[W.WindSlot != 0 ? WindIndices.y : WindIndices.x];
	wind[entry * 3] = float4(0.0f, W.WindMagnitude, faded, leafFrequency);
	wind[entry * 3 + 1] = float4(timer * W.TimerScale, previousTimer * W.TimerScale, distance2, amplitude);
	wind[entry * 3 + 2] = float4(asfloat(generation), 0.0f, 0.0f, 0.0f);
}
