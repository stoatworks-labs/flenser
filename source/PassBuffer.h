#pragma once

#include <FFGLSDK.h>

namespace flenser
{
/**
	An off-screen buffer for one stage of the chain.

	Three things on top of the SDK's `FFGLFBO`.

	**It reallocates only when it has to.** `Ensure()` is called every frame
	for every buffer and is a no-op in the overwhelming majority of them.

	**It actually frees its colour texture.** `ffglex::FFGLFBO::Release()`
	deletes the framebuffer and the depth renderbuffer, then tests
	`depthBufferID` a second time where it plainly meant `colorTextureID` --
	so the colour texture is leaked on every release (SDK b1afaf9,
	`FFGLFBO.cpp`). `Destroy()` deletes it first. Four buffers leaking one
	texture each per resize is a slow leak rather than a fast one, which is
	worse: it surfaces during a show rather than during a test.

	**It owns its filtering.** Every buffer here is a picture read between
	texels -- the simmer pass samples the feedback buffer at a displaced
	coordinate, and the composite samples both at the same one -- so they all
	want `GL_LINEAR`. `Nearest` is offered anyway, because a buffer that is
	data rather than a picture is the usual reason to add a pass, and the trap
	when that day comes is silent: `GL_LINEAR` on a data buffer does not fail,
	it returns plausible averages of unrelated numbers.

	Mipmapping is offered and nothing here uses it. Flenser samples at one
	scale throughout; the level exists because the option is one line and
	discovering it is missing is not.
*/
class PassBuffer : public ffglex::FFGLFBO
{
public:
	enum class Sampling
	{
		Nearest,  ///< for data read texel-for-texel. No filtering, no mip chain.
		Linear,   ///< for pictures read between texels. Bilinear, no mip chain.
		Mipmapped ///< for pictures that also get reduced. Trilinear + GenerateMipmaps().
	};

	~PassBuffer();

	/// Allocate at this size and format, reusing the existing buffer if it
	/// already matches. Newly allocated buffers are cleared: a buffer whose
	/// contents are undefined is not "a bit of noise on the first frame", it
	/// is whatever texture memory the driver handed back -- and the feedback
	/// buffer is shown to the operator on the frame after it is allocated.
	bool Ensure( GLsizei requestedWidth, GLsizei requestedHeight, GLint format, Sampling sampling );

	/// Rebuild the mip chain from level 0. A no-op on anything not allocated
	/// as `Mipmapped`.
	void GenerateMipmaps();

	/// Clear to transparent black.
	void Clear();

	/// The colour texture, for binding as an input to a later pass.
	///
	/// The SDK keeps `colorTextureID` protected and offers only
	/// `GetTextureInfo()`, which builds and returns an `FFGLTextureStruct` --
	/// six fields assembled to reach one of them, at every bind of every pass
	/// of every frame.
	GLuint TextureID() const
	{
		return colorTextureID;
	}

	GLsizei Width() const
	{
		return width;
	}

	GLsizei Height() const
	{
		return height;
	}

	/// Release everything, including the colour texture the SDK forgets.
	void Destroy();

	bool IsValid() const
	{
		return GetGLID() != 0;
	}

private:
	Sampling sampling = Sampling::Nearest;
};

} // namespace flenser
