
/*
 * Licensed Materials - Property of IBM
 *
 * trousers - An open source TCG Software Stack
 *
 * (C) Copyright International Business Machines Corp. 2004-2006
 *
 */


#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "trousers/tss.h"
#include "trousers/trousers.h"
#include "trousers_types.h"
#include "spi_internal_types.h"
#include "spi_utils.h"
#include "capabilities.h"
#include "tsplog.h"
#include "tcs_tsp.h"
#include "tspps.h"
#include "hosttable.h"
#include "tcsd_wrap.h"
#include "tcsd.h"
#include "obj.h"


TSS_RESULT
Tspi_Context_Create(TSS_HCONTEXT * phContext)	/* out */
{
	if (phContext == NULL)
		return TSPERR(TSS_E_BAD_PARAMETER);

	return obj_context_add(phContext);
}

TSS_RESULT
Tspi_Context_Close(TSS_HCONTEXT tspContext)	/* in */
{
	TCS_CONTEXT_HANDLE tcsContext;
	TSS_RESULT result;

	/* Get the TCS context, if we're connected */
	if ((result = obj_context_is_connected(tspContext, &tcsContext)))
		return result;

	/* Have the TCS do its thing */
	TCS_CloseContext(tcsContext);

	/* Note: Memory that was returned to the app that was alloc'd by this
	 * context isn't free'd here.  Any memory that the app doesn't explicitly
	 * free is left for it to free itself. */

	/* Destroy all objects */
	obj_close_context(tspContext);

	/* close the ps file */
	ps_close();

	/* We're not a connected context, so just exit */
	return TSS_SUCCESS;
}

TSS_RESULT
Tspi_Context_Connect(TSS_HCONTEXT tspContext,	/* in */
		     UNICODE *wszDestination)	/* in */
{
	TSS_RESULT result;
	TCS_CONTEXT_HANDLE tcsHandle;
	BYTE *machine_name = NULL;
	TSS_HPOLICY hPolicy;
	TSS_HOBJECT hTpm;
	UINT32 string_len = 0;

	/* see if we've already called connect with this context */
	if ((result = obj_context_is_connected(tspContext, &tcsHandle)) == TSS_SUCCESS) {
		LogError("attempted to call %s on an already connected "
			 "context!", __FUNCTION__);
		return TSPERR(TSS_E_CONNECTION_FAILED);
	} else if (result != TSPERR(TSS_E_NO_CONNECTION)) {
		return result;
	}

	if (wszDestination == NULL) {
		if ((result = obj_context_get_machine_name(tspContext,
							   &string_len,
							   &machine_name)))
			return result;

		if ((result = TCS_OpenContext_RPC(machine_name, &tcsHandle,
						  CONNECTION_TYPE_TCP_PERSISTANT)))
			return result;
	} else {
		if ((machine_name =
		    Trspi_UNICODE_To_Native((BYTE *)wszDestination, NULL)) == NULL) {
			LogError("Error converting hostname to UTF-8");
			return TSPERR(TSS_E_INTERNAL_ERROR);
		}

		if ((result = TCS_OpenContext_RPC(machine_name, &tcsHandle,
						CONNECTION_TYPE_TCP_PERSISTANT)))
			return result;

		if ((result = obj_context_set_machine_name(tspContext, machine_name,
						strlen((char *)machine_name)+1)))
			return result;
	}

        /* Assign an empty policy to this new object */
        if ((obj_policy_add(tspContext, TSS_POLICY_USAGE, &hPolicy)))
                return TSPERR(TSS_E_INTERNAL_ERROR);

        obj_context_set_policy(tspContext, hPolicy);

        if ((obj_tpm_add(tspContext, &hTpm)))
                return TSPERR(TSS_E_INTERNAL_ERROR);

        obj_connectContext(tspContext, tcsHandle);

	return TSS_SUCCESS;
}

TSS_RESULT
Tspi_Context_FreeMemory(TSS_HCONTEXT tspContext,	/* in */
			BYTE * rgbMemory)		/* in */
{
	if (!obj_is_context(tspContext))
		return TSPERR(TSS_E_INVALID_HANDLE);

	return free_tspi(tspContext, rgbMemory);
}

TSS_RESULT
Tspi_Context_GetDefaultPolicy(TSS_HCONTEXT tspContext,	/* in */
			      TSS_HPOLICY * phPolicy)	/* out */
{
	if (phPolicy == NULL )
		return TSPERR(TSS_E_BAD_PARAMETER);

	if (!obj_is_context(tspContext))
		return TSPERR(TSS_E_INVALID_HANDLE);

	return obj_context_get_policy(tspContext, phPolicy);
}

TSS_RESULT
Tspi_Context_CreateObject(TSS_HCONTEXT tspContext,	/* in */
			  TSS_FLAG objectType,		/* in */
			  TSS_FLAG initFlags,		/* in */
			  TSS_HOBJECT * phObject)	/* out */
{
	TSS_RESULT result;

	if (phObject == NULL)
		return TSPERR(TSS_E_BAD_PARAMETER);

	if (!obj_is_context(tspContext))
		return TSPERR(TSS_E_INVALID_HANDLE);

	switch (objectType) {
	case TSS_OBJECT_TYPE_POLICY:
		switch (initFlags) {
			case TSS_POLICY_MIGRATION:
				/* fall through */
			case TSS_POLICY_USAGE:
				break;
			default:
				return TSPERR(TSS_E_INVALID_OBJECT_INITFLAG);
		}

		result = obj_policy_add(tspContext, initFlags, phObject);
		break;
	case TSS_OBJECT_TYPE_RSAKEY:
		/* If other flags are set that disagree with the SRK, this will
		 * help catch that conflict in the later steps */
		if (initFlags & TSS_KEY_TSP_SRK) {
			initFlags |= (TSS_KEY_TYPE_STORAGE |
				      TSS_KEY_NOT_MIGRATABLE |
				      TSS_KEY_NON_VOLATILE | TSS_KEY_SIZE_2048 |
				      TSS_KEY_AUTHORIZATION);
		}

		/* Set default key flags */

		/* Default key size = 2k */
		if ((initFlags & TSS_KEY_SIZE_MASK) == 0)
			initFlags |= TSS_KEY_SIZE_2048;

		/* Default key type = storage */
		if ((initFlags & TSS_KEY_TYPE_MASK) == 0)
			initFlags |= TSS_KEY_TYPE_STORAGE;

		/* Check the key flags */
		switch (initFlags & TSS_KEY_SIZE_MASK) {
			case TSS_KEY_SIZE_512:
				/* fall through */
			case TSS_KEY_SIZE_1024:
				/* fall through */
			case TSS_KEY_SIZE_2048:
				/* fall through */
			case TSS_KEY_SIZE_4096:
				/* fall through */
			case TSS_KEY_SIZE_8192:
				/* fall through */
			case TSS_KEY_SIZE_16384:
				break;
			default:
				return TSPERR(TSS_E_INVALID_OBJECT_INITFLAG);
		}

		switch (initFlags & TSS_KEY_TYPE_MASK) {
			case TSS_KEY_TYPE_STORAGE:
				/* fall through */
			case TSS_KEY_TYPE_SIGNING:
				/* fall through */
			case TSS_KEY_TYPE_BIND:
				/* fall through */
			case TSS_KEY_TYPE_AUTHCHANGE:
				/* fall through */
			case TSS_KEY_TYPE_LEGACY:
				/* fall through */
			case TSS_KEY_TYPE_IDENTITY:
				break;
			default:
				return TSPERR(TSS_E_INVALID_OBJECT_INITFLAG);
		}

		result = obj_rsakey_add(tspContext, initFlags, phObject);
		break;
	case TSS_OBJECT_TYPE_ENCDATA:
		switch (initFlags & TSS_ENCDATA_TYPE_MASK) {
			case TSS_ENCDATA_LEGACY:
				/* fall through */
			case TSS_ENCDATA_SEAL:
				/* fall through */
			case TSS_ENCDATA_BIND:
				break;
			default:
				return TSPERR(TSS_E_INVALID_OBJECT_INITFLAG);
		}

		result = obj_encdata_add(tspContext, (initFlags & TSS_ENCDATA_TYPE_MASK),
					 phObject);
		break;
	case TSS_OBJECT_TYPE_PCRS:
		/* There are no valid flags for a PCRs object */
		if (initFlags & ~(0UL))
			return TSPERR(TSS_E_INVALID_OBJECT_INITFLAG);

		result = obj_pcrs_add(tspContext, phObject);
		break;
	case TSS_OBJECT_TYPE_HASH:
		switch (initFlags) {
			case TSS_HASH_DEFAULT:
				/* fall through */
			case TSS_HASH_SHA1:
				/* fall through */
			case TSS_HASH_OTHER:
				break;
			default:
				return TSPERR(TSS_E_INVALID_OBJECT_INITFLAG);
		}

		result = obj_hash_add(tspContext, initFlags, phObject);
		break;
	default:
		LogDebug("Invalid Object type");
		return TSPERR(TSS_E_INVALID_OBJECT_TYPE);
		break;
	}

	return result;
}

TSS_RESULT
Tspi_Context_CloseObject(TSS_HCONTEXT tspContext,	/* in */
			 TSS_HOBJECT hObject)		/* in */
{
	TSS_RESULT result;

	if (!obj_is_context(tspContext))
		return TSPERR(TSS_E_INVALID_HANDLE);

	if (obj_is_pcrs(hObject)) {
		result = obj_pcrs_remove(hObject, tspContext);
	} else if (obj_is_encdata(hObject)) {
		result = obj_encdata_remove(hObject, tspContext);
	} else if (obj_is_hash(hObject)) {
		result = obj_hash_remove(hObject, tspContext);
	} else if (obj_is_rsakey(hObject)) {
		result = obj_rsakey_remove(hObject, tspContext);
	} else if (obj_is_policy(hObject)) {
		result = obj_policy_remove(hObject, tspContext);
	} else {
		result = TSPERR(TSS_E_INVALID_HANDLE);
	}

	return result;
}

TSS_RESULT
Tspi_Context_GetTpmObject(TSS_HCONTEXT tspContext,	/*  in */
			  TSS_HTPM * phTPM)		/*  out */
{
	if (phTPM == NULL)
		return TSPERR(TSS_E_BAD_PARAMETER);

	if (!obj_is_context(tspContext))
		return TSPERR(TSS_E_INVALID_HANDLE);

	return obj_tpm_get(tspContext, phTPM);
}

TSS_RESULT
Tspi_Context_GetCapability(TSS_HCONTEXT tspContext,	/* in */
			   TSS_FLAG capArea,		/* in */
			   UINT32 ulSubCapLength,	/* in */
			   BYTE * rgbSubCap,		/* in */
			   UINT32 * pulRespDataLength,	/* out */
			   BYTE ** prgbRespData)	/* out */
{
	TSS_RESULT result;
	TCS_CONTEXT_HANDLE tcsContext;
	UINT32 subCap;

	if (prgbRespData == NULL || pulRespDataLength == NULL )
		return TSPERR(TSS_E_BAD_PARAMETER);

	if (rgbSubCap == NULL && ulSubCapLength != 0)
		return TSPERR(TSS_E_BAD_PARAMETER);

	if (ulSubCapLength > sizeof(UINT32))
		return TSPERR(TSS_E_BAD_PARAMETER);

	if (!obj_is_context(tspContext))
		return TSPERR(TSS_E_INVALID_HANDLE);

	switch (capArea) {
		case TSS_TSPCAP_ALG:
		case TSS_TSPCAP_VERSION:
		case TSS_TSPCAP_PERSSTORAGE:
			if (capArea == TSS_TSPCAP_ALG) {
				if (ulSubCapLength != sizeof(UINT32) || !rgbSubCap)
					return TSPERR(TSS_E_BAD_PARAMETER);
			}

			result = internal_GetCap(tspContext, capArea,
						 rgbSubCap ? *(UINT32 *)rgbSubCap : 0,
						 pulRespDataLength,
						 prgbRespData);
			break;
		case TSS_TCSCAP_ALG:
		case TSS_TCSCAP_VERSION:
		case TSS_TCSCAP_CACHING:
		case TSS_TCSCAP_PERSSTORAGE:
		case TSS_TCSCAP_MANUFACTURER:
			/* make sure we're connected to a TCS first */
			if ((result = obj_context_is_connected(tspContext,
							&tcsContext)))
				return result;

			if (capArea == TSS_TCSCAP_ALG) {
				if (ulSubCapLength != sizeof(UINT32) || !rgbSubCap)
					return TSPERR(TSS_E_BAD_PARAMETER);
			}

			subCap = rgbSubCap ? endian32(*(UINT32 *)rgbSubCap) : 0;

			result = TCS_GetCapability(tcsContext,
							capArea,
							ulSubCapLength,
							(BYTE *)&subCap,
							pulRespDataLength,
							prgbRespData);
			break;
		default:
			result = TSPERR(TSS_E_BAD_PARAMETER);
			break;
	}

	return result;
}

TSS_RESULT
Tspi_Context_LoadKeyByBlob(TSS_HCONTEXT tspContext,	/* in */
			   TSS_HKEY hUnwrappingKey,	/* in */
			   UINT32 ulBlobLength,		/* in */
			   BYTE * rgbBlobData,		/* in */
			   TSS_HKEY * phKey)		/* out */
{
	TPM_AUTH auth;
	BYTE blob[1024];
	UINT16 offset;
	TCPA_DIGEST digest;
	TSS_RESULT result;
	UINT32 keyslot;
	TSS_HPOLICY hPolicy;
	TCS_CONTEXT_HANDLE tcsContext;
	TCS_KEY_HANDLE parentTCSKeyHandle;
	TCS_KEY_HANDLE myTCSKeyHandle;
	TCPA_KEY keyContainer;
	TSS_BOOL useAuth;
	TPM_AUTH *pAuth;
	TSS_FLAG initFlags;
	UINT16 realKeyBlobSize;
	TCPA_KEY_USAGE keyUsage;
	UINT32 pubLen;

	if (phKey == NULL || rgbBlobData == NULL )
		return TSPERR(TSS_E_BAD_PARAMETER);

	if (!obj_is_context(tspContext) || !obj_is_rsakey(hUnwrappingKey))
		return TSPERR(TSS_E_INVALID_HANDLE);

	/* Loading a key always requires us to be connected to a TCS */
	if ((result = obj_context_is_connected(tspContext, &tcsContext)))
		return result;

	/* Get the Parent Handle */
	if ((result = obj_rsakey_get_tcs_handle(hUnwrappingKey, &parentTCSKeyHandle)))
		return result;

	offset = 0;
	if ((result = Trspi_UnloadBlob_KEY(&offset, rgbBlobData, &keyContainer)))
		return result;
	realKeyBlobSize = offset;
	pubLen = keyContainer.pubKey.keyLength;
	keyUsage = keyContainer.keyUsage;
	/* free these now, since they're not used below */
	free_key_refs(&keyContainer);

	if ((result = obj_rsakey_get_policy(hUnwrappingKey, TSS_POLICY_USAGE,
					&hPolicy, &useAuth)))
		return result;

	if (useAuth) {
		/* ---  Create the Authorization */
		offset = 0;
		Trspi_LoadBlob_UINT32(&offset, TPM_ORD_LoadKey, blob);
		Trspi_LoadBlob(&offset, ulBlobLength, blob, rgbBlobData);
		Trspi_Hash(TSS_HASH_SHA1, offset, blob, digest.digest);

		if ((result = secret_PerformAuth_OIAP(hUnwrappingKey, TPM_ORD_LoadKey,
						      hPolicy, &digest, &auth)))
			return result;

		pAuth = &auth;
	} else {
		pAuth = NULL;
	}

	if ((result = TCSP_LoadKeyByBlob(tcsContext, parentTCSKeyHandle,
					ulBlobLength, rgbBlobData,
					pAuth, &myTCSKeyHandle, &keyslot)))
		return result;

	if (useAuth) {
		/* ---  Validate return auth */
		offset = 0;
		Trspi_LoadBlob_UINT32(&offset, result, blob);
		Trspi_LoadBlob_UINT32(&offset, TPM_ORD_LoadKey, blob);
		Trspi_LoadBlob_UINT32(&offset, keyslot, blob);
		Trspi_Hash(TSS_HASH_SHA1, offset, blob, digest.digest);

		if ((result = obj_policy_validate_auth_oiap(hPolicy, &digest, &auth)))
			return result;
	}

	/* ---  Create a new Object */
	initFlags = 0;
	if (pubLen == 0x100)
		initFlags |= TSS_KEY_SIZE_2048;
	else if (pubLen == 0x80)
		initFlags |= TSS_KEY_SIZE_1024;
	else if (pubLen == 0x40)
		initFlags |= TSS_KEY_SIZE_512;

	/* clear the key type field */
	initFlags &= ~TSS_KEY_TYPE_MASK;

	if (keyUsage == TPM_KEY_STORAGE)
		initFlags |= TSS_KEY_TYPE_STORAGE;
	else
		initFlags |= TSS_KEY_TYPE_SIGNING;	/* loading the blob
							   will fix this
							   back to what it
							   should be. */

	if ((result = obj_rsakey_add(tspContext, initFlags, phKey))) {
		LogDebug("Failed create object");
		return TSPERR(TSS_E_INTERNAL_ERROR);
	}

	if ((result = obj_rsakey_set_tcpakey(*phKey,realKeyBlobSize, rgbBlobData))) {
		LogDebug("Key loaded but failed to setup the key object"
			  "correctly");
		return TSPERR(TSS_E_INTERNAL_ERROR);
	}

	return obj_rsakey_set_tcs_handle(*phKey, myTCSKeyHandle);
}

TSS_RESULT
Tspi_Context_LoadKeyByUUID(TSS_HCONTEXT tspContext,		/* in */
			   TSS_FLAG persistentStorageType,	/* in */
			   TSS_UUID uuidData,			/* in */
			   TSS_HKEY * phKey)			/* out */
{
	TSS_RESULT result;
	TSS_UUID parentUUID;
	TCS_CONTEXT_HANDLE tcsContext;
	UINT32 keyBlobSize, parentPSType;
	BYTE *keyBlob = NULL;
	TCS_KEY_HANDLE tcsKeyHandle;
	TSS_HKEY parentTspHandle;
	TCS_KEY_HANDLE parentTCSKeyHandle;
	TCS_LOADKEY_INFO info;

	if (phKey == NULL)
		return TSPERR(TSS_E_BAD_PARAMETER);

	/* Loading a key always requires us to be connected to a TCS */
	if ((result = obj_context_is_connected(tspContext, &tcsContext)))
		return result;

	memset(&info, 0, sizeof(TCS_LOADKEY_INFO));

	/* This key is in the System Persistant storage */
	if (persistentStorageType == TSS_PS_TYPE_SYSTEM) {
		memset(&info, 0, sizeof(TCS_LOADKEY_INFO));

		result = TCSP_LoadKeyByUUID(tcsContext,
					    uuidData,
					    &info,
					    &tcsKeyHandle);

		if (TSS_ERROR_CODE(result) == TCS_E_KM_LOADFAILED) {
			TSS_HKEY keyHandle;
			TSS_HPOLICY hPolicy;

			/* load failed, due to some key in the chain needing auth
			 * which doesn't yet exist at the TCS level. However, the
			 * auth may already be set in policies at the TSP level.
			 * To find out, get the key handle of the key requiring
			 * auth */
			if (ps_get_key_by_uuid(tspContext, &info.parentKeyUUID, &keyHandle))
				return result;

			if (obj_rsakey_get_policy(keyHandle, TSS_POLICY_USAGE,
						  &hPolicy, NULL))
				return result;

			if (secret_PerformAuth_OIAP(keyHandle,
						    TPM_ORD_LoadKey,
						    hPolicy, &info.paramDigest,
						    &info.authData))
				return result;

			if ((result = TCSP_LoadKeyByUUID(tcsContext, uuidData,
							 &info, &tcsKeyHandle)))
				return result;
		} else if (result)
			return result;

		if ((result = TCS_GetRegisteredKeyBlob(tcsContext,
						       uuidData,
						       &keyBlobSize,
						       &keyBlob)))
			return result;

		if ((result = obj_rsakey_add_by_key(tspContext, &uuidData, keyBlob,
						    TSS_OBJ_FLAG_SYSTEM_PS, phKey))) {
			free (keyBlob);
			return result;
		}

		result = obj_rsakey_set_tcs_handle(*phKey, tcsKeyHandle);

		free (keyBlob);
	} else if (persistentStorageType == TSS_PS_TYPE_USER) {
		if ((result = ps_get_parent_uuid_by_uuid(&uuidData, &parentUUID)))
			return result;

		if ((result = ps_get_parent_ps_type_by_uuid(&uuidData, &parentPSType)))
			return result;

		/******************************************
		 * If the parent is system persistant, then just call
		 *  the TCS's LoadKeyByUUID function.
		 * If the parent is in user storage, then we need to
		 *  call Tspi_LoadKeyByUUID and get a parentKeyObject.
		 *  This object can then be translated into a
		 *  TCS_KEY_HANDLE.
		 ******************************************/

		if (parentPSType == TSS_PS_TYPE_SYSTEM) {
			if ((result = TCSP_LoadKeyByUUID(tcsContext, parentUUID, NULL,
							 &parentTCSKeyHandle)))
				return result;
		} else if (parentPSType == TSS_PS_TYPE_USER) {
			if ((result = Tspi_Context_LoadKeyByUUID(tspContext, parentPSType,
								 parentUUID, &parentTspHandle)))
				return result;

			/* Get the parentTCS Key Handle from our table */
			if ((result = obj_rsakey_get_tcs_handle(parentTspHandle,
								&parentTCSKeyHandle)))
				return result;

			/* Close the object since it's not needed
			 * anymore */
			obj_rsakey_remove(parentTspHandle, tspContext);
		} else {
			return TSPERR(TSS_E_BAD_PARAMETER);
		}

		/* Get my KeyBlob */
		if ((result = ps_get_key_by_uuid(tspContext, &uuidData, phKey)))
			return result;

		/*******************************
		 * Now the parent is loaded and we have the parent key
		 * handle call the TCS to actually load the key now.
		 ******************************/
		return Tspi_Key_LoadKey(*phKey, parentTspHandle);
	} else {
		return TSPERR(TSS_E_BAD_PARAMETER);
	}

	return TSS_SUCCESS;
}

TSS_RESULT
Tspi_Context_RegisterKey(TSS_HCONTEXT tspContext,		/* in */
			 TSS_HKEY hKey,				/* in */
			 TSS_FLAG persistentStorageType,	/* in */
			 TSS_UUID uuidKey,			/* in */
			 TSS_FLAG persistentStorageTypeParent,	/* in */
			 TSS_UUID uuidParentKey)		/* in */
{
	BYTE *keyBlob;
	UINT32 keyBlobSize;
	TSS_RESULT result;
	TSS_BOOL answer;
	TCS_CONTEXT_HANDLE tcsContext;

	if (!obj_is_context(tspContext) || !obj_is_rsakey(hKey))
		return TSPERR(TSS_E_INVALID_HANDLE);

	if (persistentStorageType == TSS_PS_TYPE_SYSTEM) {
		/* make sure we're connected to a TCS */
		if ((result = obj_context_is_connected(tspContext, &tcsContext)))
			return result;

		if (persistentStorageTypeParent == TSS_PS_TYPE_USER) {
			return TSPERR(TSS_E_NOTIMPL);
		} else if (persistentStorageTypeParent == TSS_PS_TYPE_SYSTEM) {
			if ((result = obj_rsakey_get_blob(hKey, &keyBlobSize,
							  &keyBlob)))
				return result;

			if ((result = TCS_RegisterKey(tcsContext,
						     uuidParentKey,
						     uuidKey,
						     keyBlobSize,
						     keyBlob,
						     strlen(PACKAGE_STRING) + 1,
						     (BYTE *)PACKAGE_STRING)))
				return result;
		} else {
			return TSPERR(TSS_E_BAD_PARAMETER);
		}
	} else if (persistentStorageType == TSS_PS_TYPE_USER) {
		if ((result = ps_is_key_registered(&uuidKey, &answer)))
			return result;

		if (answer == TRUE)
			return TSPERR(TSS_E_KEY_ALREADY_REGISTERED);

		if ((result = obj_rsakey_get_blob (hKey, &keyBlobSize, &keyBlob)))
			return result;

		if ((result = ps_write_key(&uuidKey, &uuidParentKey,
					   persistentStorageTypeParent,
					   keyBlobSize, keyBlob)))
			return result;
	} else {
		return TSPERR(TSS_E_BAD_PARAMETER);
	}

	if ((result = obj_rsakey_set_uuid(hKey, persistentStorageType, &uuidKey)))
		return result;

	return TSS_SUCCESS;
}

TSS_RESULT
Tspi_Context_UnregisterKey(TSS_HCONTEXT tspContext,		/* in */
			   TSS_FLAG persistentStorageType,	/* in */
			   TSS_UUID uuidKey,			/* in */
			   TSS_HKEY *phKey)			/* out */
{
	TCS_CONTEXT_HANDLE tcsContext;
	BYTE *keyBlob = NULL;
	UINT32 keyBlobSize;
	TSS_RESULT result;

	if (phKey == NULL)
		return TSPERR(TSS_E_BAD_PARAMETER);

	if (persistentStorageType == TSS_PS_TYPE_SYSTEM) {
		/* make sure we're connected to a TCS first */
		if ((result = obj_context_is_connected(tspContext, &tcsContext)))
			return result;

		/* get the key first, so it doesn't disappear when we
		 * unregister it */
		if ((result = TCS_GetRegisteredKeyBlob(tcsContext, uuidKey, &keyBlobSize,
						       &keyBlob)))
			return result;

		if ((obj_rsakey_add_by_key(tspContext, &uuidKey, keyBlob, TSS_OBJ_FLAG_SYSTEM_PS,
					   phKey))) {
			free(keyBlob);
			return result;
		}

		free(keyBlob);

		/* now unregister it */
		if ((result = TCSP_UnregisterKey(tcsContext, uuidKey)))
			return result;
	} else if (persistentStorageType == TSS_PS_TYPE_USER) {
		if (!obj_is_context(tspContext))
			return TSPERR(TSS_E_INVALID_HANDLE);

		/* get the key first, so it doesn't disappear when we
		 * unregister it */
		if ((result = ps_get_key_by_uuid(tspContext, &uuidKey, phKey)))
			return result;

		/* now unregister it */
		if ((result = ps_remove_key(&uuidKey)))
			return result;
	} else {
		return TSPERR(TSS_E_BAD_PARAMETER);
	}

	return TSS_SUCCESS;
}

TSS_RESULT
Tspi_Context_GetKeyByUUID(TSS_HCONTEXT tspContext,		/* in */
			  TSS_FLAG persistentStorageType,	/* in */
			  TSS_UUID uuidData,			/* in */
			  TSS_HKEY * phKey)			/* out */
{
	TCPA_RESULT result;
	UINT32 keyBlobSize = 0;
	BYTE *keyBlob = NULL;
	TCS_CONTEXT_HANDLE tcsContext;

	if (phKey == NULL)
		return TSPERR(TSS_E_BAD_PARAMETER);

	if (persistentStorageType == TSS_PS_TYPE_SYSTEM) {
		/* make sure we're connected to a TCS first */
		if ((result = obj_context_is_connected(tspContext, &tcsContext)))
			return result;

		if ((result = TCS_GetRegisteredKeyBlob(tcsContext, uuidData,
						       &keyBlobSize,
						       &keyBlob)))
			return result;

		if ((obj_rsakey_add_by_key(tspContext, &uuidData, keyBlob, TSS_OBJ_FLAG_SYSTEM_PS,
					   phKey))) {
			free(keyBlob);
			return result;
		}

		free(keyBlob);
	} else if (persistentStorageType == TSS_PS_TYPE_USER) {
		if (!obj_is_context(tspContext))
			return TSPERR(TSS_E_INVALID_HANDLE);

		if ((result = ps_get_key_by_uuid(tspContext, &uuidData, phKey)))
			return result;
	} else
		return TSPERR(TSS_E_BAD_PARAMETER);

	return TSS_SUCCESS;
}

TSS_RESULT
Tspi_Context_GetKeyByPublicInfo(TSS_HCONTEXT tspContext,	/* in */
				TSS_FLAG persistentStorageType,	/* in */
				TSS_ALGORITHM_ID algID,		/* in */
				UINT32 ulPublicInfoLength,	/* in */
				BYTE * rgbPublicInfo,		/* in */
				TSS_HKEY * phKey)		/* out */
{
	TCS_CONTEXT_HANDLE tcsContext;
	TCPA_ALGORITHM_ID tcsAlgID;
	UINT32 keyBlobSize;
	BYTE *keyBlob;
	TSS_RESULT result;
	TSS_HKEY keyOutHandle;
	UINT32 flag = 0;
	TCPA_KEY keyContainer;
	UINT16 offset;

	if (phKey == NULL)
		return TSPERR(TSS_E_BAD_PARAMETER);

	if (!obj_is_context(tspContext))
		return TSPERR(TSS_E_INVALID_HANDLE);

	switch (algID) {
		case TSS_ALG_RSA:
			tcsAlgID = TCPA_ALG_RSA;
			break;
		default:
			LogError("Algorithm ID was not type RSA.");
			return TSPERR(TSS_E_BAD_PARAMETER);
	}

	if (persistentStorageType == TSS_PS_TYPE_SYSTEM) {
		/* make sure we're connected to a TCS */
		if ((result = obj_context_is_connected(tspContext, &tcsContext)))
			return result;

		if ((result = TCSP_GetRegisteredKeyByPublicInfo(tcsContext,
								tcsAlgID,
								ulPublicInfoLength,
								rgbPublicInfo,
								&keyBlobSize,
								&keyBlob)))
			return result;

	} else if (persistentStorageType == TSS_PS_TYPE_USER) {
		return ps_get_key_by_pub(tspContext, ulPublicInfoLength, rgbPublicInfo,
					 phKey);
	} else
		return TSPERR(TSS_E_BAD_PARAMETER);

	/* need to setup the init flags of the create object based on
	 * the size of the blob's pubkey */
	offset = 0;
	if ((result = Trspi_UnloadBlob_KEY(&offset, keyBlob, &keyContainer))) {
		free(keyBlob);
		return result;
	}

	/* begin setting up the key object */
	switch (keyContainer.pubKey.keyLength) {
		case 16384/8:
			flag |= TSS_KEY_SIZE_16384;
			break;
		case 8192/8:
			flag |= TSS_KEY_SIZE_8192;
			break;
		case 4096/8:
			flag |= TSS_KEY_SIZE_4096;
			break;
		case 2048/8:
			flag |= TSS_KEY_SIZE_2048;
			break;
		case 1024/8:
			flag |= TSS_KEY_SIZE_1024;
			break;
		case 512/8:
			flag |= TSS_KEY_SIZE_512;
			break;
		default:
			LogError("Key was not a known keylength.");
			free(keyBlob);
			free_key_refs(&keyContainer);
			return TSPERR(TSS_E_INTERNAL_ERROR);
	}

	if (keyContainer.keyUsage == TPM_KEY_SIGNING)
		flag |= TSS_KEY_TYPE_SIGNING;
	else if (keyContainer.keyUsage == TPM_KEY_STORAGE)
		flag |= TSS_KEY_TYPE_STORAGE;
	else if (keyContainer.keyUsage == TPM_KEY_IDENTITY)
		flag |= TSS_KEY_TYPE_IDENTITY;
	else if (keyContainer.keyUsage == TPM_KEY_AUTHCHANGE)
		flag |= TSS_KEY_TYPE_AUTHCHANGE;
	else if (keyContainer.keyUsage == TPM_KEY_BIND)
		flag |= TSS_KEY_TYPE_BIND;
	else if (keyContainer.keyUsage == TPM_KEY_LEGACY)
		flag |= TSS_KEY_TYPE_LEGACY;

	if (keyContainer.authDataUsage == TPM_AUTH_NEVER)
		flag |= TSS_KEY_NO_AUTHORIZATION;
	else
		flag |= TSS_KEY_AUTHORIZATION;

	if (keyContainer.keyFlags & migratable)
		flag |= TSS_KEY_MIGRATABLE;
	else
		flag |= TSS_KEY_NOT_MIGRATABLE;

	if (keyContainer.keyFlags & volatileKey)
		flag |= TSS_KEY_VOLATILE;
	else
		flag |= TSS_KEY_NON_VOLATILE;

	/* Create a new Key Object */
	if ((result = obj_rsakey_add(tspContext, flag, &keyOutHandle))) {
		free(keyBlob);
		free_key_refs(&keyContainer);
		return result;
	}
	/* Stick the info into this net KeyObject */
	if ((result = obj_rsakey_set_tcpakey(keyOutHandle, keyBlobSize, keyBlob))) {
		free(keyBlob);
		free_key_refs(&keyContainer);
		return result;
	}

	free(keyBlob);
	free_key_refs(&keyContainer);
	*phKey = keyOutHandle;

	return TSS_SUCCESS;
}

TSS_RESULT
Tspi_Context_GetRegisteredKeysByUUID(TSS_HCONTEXT tspContext,		/* in */
				     TSS_FLAG persistentStorageType,	/* in */
				     TSS_UUID * pUuidData,		/* in */
				     UINT32 * pulKeyHierarchySize,	/* out */
				     TSS_KM_KEYINFO ** ppKeyHierarchy)	/* out */
{
	TSS_RESULT result;
	TCS_CONTEXT_HANDLE tcsContext;
	TSS_KM_KEYINFO *tcsHier, *tspHier;
	UINT32 tcsHierSize, tspHierSize;

	if (pulKeyHierarchySize == NULL || ppKeyHierarchy == NULL)
		return TSPERR(TSS_E_BAD_PARAMETER);

	if (!obj_is_context(tspContext))
		return TSPERR(TSS_E_INVALID_HANDLE);

	if (pUuidData) {
		if (persistentStorageType == TSS_PS_TYPE_SYSTEM) {
			/* make sure we're connected to a TCS */
			if ((result = obj_context_is_connected(tspContext, &tcsContext)))
				return result;

			if ((result = TCS_EnumRegisteredKeys(tcsContext, pUuidData,
							     pulKeyHierarchySize,
							     ppKeyHierarchy)))
				return result;

			if ((result = add_mem_entry(tspContext, *ppKeyHierarchy))) {
				free(*ppKeyHierarchy);
				*ppKeyHierarchy = NULL;
				*pulKeyHierarchySize = 0;
				return result;
			}

			return TSS_SUCCESS;
		} else if (persistentStorageType == TSS_PS_TYPE_USER) {
			if ((result = ps_get_registered_keys(pUuidData, pulKeyHierarchySize,
							     ppKeyHierarchy)))
				return result;

			if ((result = add_mem_entry(tspContext, *ppKeyHierarchy))) {
				free(*ppKeyHierarchy);
				*ppKeyHierarchy = NULL;
				*pulKeyHierarchySize = 0;
				return result;
			}

			return TSS_SUCCESS;
		} else
			return TSPERR(TSS_E_BAD_PARAMETER);
	} else {
		/* make sure we're connected to a TCS */
		if ((result = obj_context_is_connected(tspContext, &tcsContext)))
			return result;

		if ((result = TCS_EnumRegisteredKeys(tcsContext, pUuidData, &tcsHierSize,
						     &tcsHier)))
			return result;

		if ((result = ps_get_registered_keys(pUuidData, &tspHierSize, &tspHier))) {
			free(tcsHier);
			return result;
		}

		if ((result = merge_key_hierarchies(tspContext, tspHierSize, tspHier, tcsHierSize,
						    tcsHier, pulKeyHierarchySize,
						    ppKeyHierarchy))) {
			free(tcsHier);
			free(tspHier);
			return result;
		}
	}

	return TSS_SUCCESS;
}
