// SPDX-License-Identifier: GPL-2.0

#include <drm/drm_syncobj.h>

#ifdef CONFIG_DRM

__rust_helper void rust_helper_drm_syncobj_get(struct drm_syncobj *obj)
{
	drm_syncobj_get(obj);
}

__rust_helper void rust_helper_drm_syncobj_put(struct drm_syncobj *obj)
{
	drm_syncobj_put(obj);
}

__rust_helper struct dma_fence *rust_helper_drm_syncobj_fence_get(struct drm_syncobj *syncobj)
{
	return drm_syncobj_fence_get(syncobj);
}

#endif
