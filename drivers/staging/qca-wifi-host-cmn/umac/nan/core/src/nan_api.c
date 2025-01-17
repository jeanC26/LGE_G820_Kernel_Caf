/*
 * Copyright (c) 2016-2018 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * DOC: contains nan public API function definitions
 */

#include "nan_main_i.h"
#include "wlan_nan_api.h"
#include "target_if_nan.h"
#include "nan_public_structs.h"
#include "wlan_objmgr_cmn.h"
#include "wlan_objmgr_global_obj.h"
#include "wlan_objmgr_psoc_obj.h"
#include "wlan_objmgr_pdev_obj.h"
#include "wlan_objmgr_vdev_obj.h"
#include "nan_ucfg_api.h"

static QDF_STATUS nan_psoc_obj_created_notification(
		struct wlan_objmgr_psoc *psoc, void *arg_list)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	struct nan_psoc_priv_obj *nan_obj;

	nan_debug("nan_psoc_create_notif called");
	nan_obj = qdf_mem_malloc(sizeof(*nan_obj));
	if (!nan_obj) {
		nan_alert("malloc failed for nan prv obj");
		return QDF_STATUS_E_NOMEM;
	}

	qdf_spinlock_create(&nan_obj->lock);
	status = wlan_objmgr_psoc_component_obj_attach(psoc,
			WLAN_UMAC_COMP_NAN, nan_obj,
			QDF_STATUS_SUCCESS);
	if (QDF_IS_STATUS_ERROR(status)) {
		nan_alert("obj attach with psoc failed");
		goto nan_psoc_notif_failed;
	}

	return QDF_STATUS_SUCCESS;

nan_psoc_notif_failed:

	qdf_spinlock_destroy(&nan_obj->lock);
	qdf_mem_free(nan_obj);
	return status;
}

static void nan_psoc_delete_peer(struct wlan_objmgr_psoc *psoc,
				 void *peer, void *arg)
{
	if (WLAN_PEER_NDP == wlan_peer_get_peer_type(peer))
		wlan_objmgr_peer_obj_delete(peer);
}

static void nan_psoc_delete_vdev(struct wlan_objmgr_psoc *psoc,
				 void *vdev, void *arg)
{
	enum nan_datapath_state state;
	struct nan_vdev_priv_obj *priv_obj = nan_get_vdev_priv_obj(vdev);
	if (!priv_obj) {
		nan_err("priv_obj is null");
		return;
	}
	/*
	 * user may issue rrmod wlan without explictly NDI Delete.
	 * In that case pending NDI vdev state will not be DELETED/DELETEING
	 * Forcefully delete vdev object
	 */
	if (QDF_NDI_MODE != wlan_vdev_mlme_get_opmode(vdev))
		return;

	qdf_spin_lock_bh(&priv_obj->lock);
	state = priv_obj->state;
	qdf_spin_unlock_bh(&priv_obj->lock);

	/* if already in deleted or deleting state - do not delete */
	if (state == NAN_DATA_NDI_DELETED_STATE ||
	    state == NAN_DATA_NDI_DELETING_STATE)
		return;

	wlan_objmgr_vdev_obj_delete(vdev);
}

static QDF_STATUS nan_psoc_obj_destroyed_notification(
				struct wlan_objmgr_psoc *psoc, void *arg_list)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	struct nan_psoc_priv_obj *nan_obj = nan_get_psoc_priv_obj(psoc);

	nan_debug("nan_psoc_delete_notif called");
	if (!nan_obj) {
		nan_err("nan_obj is NULL");
		return QDF_STATUS_E_FAULT;
	}

	wlan_objmgr_iterate_obj_list(psoc, WLAN_PEER_OP,
				     nan_psoc_delete_peer,
				     NULL, 1, WLAN_NAN_ID);

	wlan_objmgr_iterate_obj_list(psoc, WLAN_VDEV_OP,
				     nan_psoc_delete_vdev,
				     NULL, 1, WLAN_NAN_ID);

	status = wlan_objmgr_psoc_component_obj_detach(psoc,
					WLAN_UMAC_COMP_NAN, nan_obj);
	if (QDF_IS_STATUS_ERROR(status))
		nan_err("nan_obj detach failed");

	nan_debug("nan_obj deleted with status %d", status);
	qdf_spinlock_destroy(&nan_obj->lock);
	qdf_mem_free(nan_obj);

	return status;
}

static void nan_is_ndp_peer_active(struct wlan_objmgr_pdev *pdev,
				   void *object,
				   void *arg)
{
	struct wlan_objmgr_vdev *vdev = (struct wlan_objmgr_vdev *)object;
	uint8_t *flag = (uint8_t *)arg;

	wlan_vdev_obj_lock(vdev);

	if (wlan_vdev_mlme_get_opmode(vdev) != QDF_NDI_MODE) {
		wlan_vdev_obj_unlock(vdev);
		return;
	}

	if (ucfg_nan_get_active_peers(vdev))
		*flag = true;

	wlan_vdev_obj_unlock(vdev);
}

bool nan_is_ndp_active(struct wlan_objmgr_pdev *pdev)
{
	bool flag = false;

	if (!pdev)
		return QDF_STATUS_E_INVAL;

	wlan_objmgr_pdev_iterate_obj_list(pdev, WLAN_VDEV_OP,
					  nan_is_ndp_peer_active,
					  &flag, 0, WLAN_NAN_ID);

	return flag;
}

static QDF_STATUS nan_vdev_obj_created_notification(
		struct wlan_objmgr_vdev *vdev, void *arg_list)
{
	struct nan_vdev_priv_obj *nan_obj;
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	nan_debug("nan_vdev_create_notif called");
	if (wlan_vdev_mlme_get_opmode(vdev) != QDF_NDI_MODE) {
		nan_debug("not a ndi vdev. do nothing");
		return QDF_STATUS_SUCCESS;
	}

	nan_obj = qdf_mem_malloc(sizeof(*nan_obj));
	if (!nan_obj) {
		nan_err("malloc failed for nan prv obj");
		return QDF_STATUS_E_NOMEM;
	}

	qdf_spinlock_create(&nan_obj->lock);
	status = wlan_objmgr_vdev_component_obj_attach(vdev,
			WLAN_UMAC_COMP_NAN, (void *)nan_obj,
			QDF_STATUS_SUCCESS);
	if (QDF_IS_STATUS_ERROR(status)) {
		nan_alert("obj attach with vdev failed");
		goto nan_vdev_notif_failed;
	}

	return QDF_STATUS_SUCCESS;

nan_vdev_notif_failed:

	qdf_spinlock_destroy(&nan_obj->lock);
	qdf_mem_free(nan_obj);
	return status;
}

static QDF_STATUS nan_vdev_obj_destroyed_notification(
				struct wlan_objmgr_vdev *vdev, void *arg_list)
{
	struct nan_vdev_priv_obj *nan_obj;
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	nan_debug("nan_vdev_delete_notif called");
	if (wlan_vdev_mlme_get_opmode(vdev) != QDF_NDI_MODE) {
		nan_debug("not a ndi vdev. do nothing");
		return QDF_STATUS_SUCCESS;
	}

	nan_obj = nan_get_vdev_priv_obj(vdev);
	if (!nan_obj) {
		nan_err("nan_obj is NULL");
		return QDF_STATUS_E_FAULT;
	}

	status = wlan_objmgr_vdev_component_obj_detach(vdev,
					WLAN_UMAC_COMP_NAN, nan_obj);
	if (QDF_IS_STATUS_ERROR(status))
		nan_err("nan_obj detach failed");

	nan_debug("nan_obj deleted with status %d", status);
	qdf_spinlock_destroy(&nan_obj->lock);
	qdf_mem_free(nan_obj);

	return status;
}

QDF_STATUS nan_init(void)
{
	QDF_STATUS status;

	/* register psoc create handler functions. */
	status = wlan_objmgr_register_psoc_create_handler(
		WLAN_UMAC_COMP_NAN,
		nan_psoc_obj_created_notification,
		NULL);
	if (QDF_IS_STATUS_ERROR(status)) {
		nan_err("wlan_objmgr_register_psoc_create_handler failed");
		return status;
	}

	/* register psoc delete handler functions. */
	status = wlan_objmgr_register_psoc_destroy_handler(
		WLAN_UMAC_COMP_NAN,
		nan_psoc_obj_destroyed_notification,
		NULL);
	if (QDF_IS_STATUS_ERROR(status)) {
		nan_err("wlan_objmgr_register_psoc_destroy_handler failed");
		nan_deinit();
		return status;
	}

	/* register vdev create handler functions. */
	status = wlan_objmgr_register_vdev_create_handler(
		WLAN_UMAC_COMP_NAN,
		nan_vdev_obj_created_notification,
		NULL);
	if (QDF_IS_STATUS_ERROR(status)) {
		nan_err("wlan_objmgr_register_psoc_create_handler failed");
		nan_deinit();
		return status;
	}

	/* register vdev delete handler functions. */
	status = wlan_objmgr_register_vdev_destroy_handler(
		WLAN_UMAC_COMP_NAN,
		nan_vdev_obj_destroyed_notification,
		NULL);
	if (QDF_IS_STATUS_ERROR(status)) {
		nan_err("wlan_objmgr_register_psoc_destroy_handler failed");
		nan_deinit();
		return status;
	}

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS nan_deinit(void)
{
	QDF_STATUS ret = QDF_STATUS_SUCCESS, status;

	/* register psoc create handler functions. */
	status = wlan_objmgr_unregister_psoc_create_handler(
		WLAN_UMAC_COMP_NAN,
		nan_psoc_obj_created_notification,
		NULL);
	if (QDF_IS_STATUS_ERROR(status)) {
		nan_err("wlan_objmgr_unregister_psoc_create_handler failed");
		ret = status;
	}

	/* register vdev create handler functions. */
	status = wlan_objmgr_unregister_psoc_destroy_handler(
		WLAN_UMAC_COMP_NAN,
		nan_psoc_obj_destroyed_notification,
		NULL);
	if (QDF_IS_STATUS_ERROR(status)) {
		nan_err("wlan_objmgr_deregister_psoc_destroy_handler failed");
		ret = status;
	}

	/* de-register vdev create handler functions. */
	status = wlan_objmgr_unregister_vdev_create_handler(
		WLAN_UMAC_COMP_NAN,
		nan_vdev_obj_created_notification,
		NULL);
	if (QDF_IS_STATUS_ERROR(status)) {
		nan_err("wlan_objmgr_unregister_psoc_create_handler failed");
		ret = status;
	}

	/* de-register vdev delete handler functions. */
	status = wlan_objmgr_unregister_vdev_destroy_handler(
		WLAN_UMAC_COMP_NAN,
		nan_vdev_obj_destroyed_notification,
		NULL);
	if (QDF_IS_STATUS_ERROR(status)) {
		nan_err("wlan_objmgr_deregister_psoc_destroy_handler failed");
		ret = status;
	}

	return ret;
}

QDF_STATUS nan_psoc_enable(struct wlan_objmgr_psoc *psoc)
{
	QDF_STATUS status = target_if_nan_register_events(psoc);

	if (QDF_IS_STATUS_ERROR(status))
		nan_err("target_if_nan_register_events failed");

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS nan_psoc_disable(struct wlan_objmgr_psoc *psoc)
{
	QDF_STATUS status = target_if_nan_deregister_events(psoc);

	if (QDF_IS_STATUS_ERROR(status))
		nan_err("target_if_nan_deregister_events failed");

	return QDF_STATUS_SUCCESS;
}
