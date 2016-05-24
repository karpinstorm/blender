/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/blenkernel/intern/library.c
 *  \ingroup bke
 *
 * Contains management of ID's and libraries
 * allocate and free of all library data
 */

#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>

#include "MEM_guardedalloc.h"

/* all types are needed here, in order to do memory operations */
#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_brush_types.h"
#include "DNA_camera_types.h"
#include "DNA_group_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_ipo_types.h"
#include "DNA_key_types.h"
#include "DNA_lamp_types.h"
#include "DNA_lattice_types.h"
#include "DNA_linestyle_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meta_types.h"
#include "DNA_movieclip_types.h"
#include "DNA_mask_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_speaker_types.h"
#include "DNA_sound_types.h"
#include "DNA_text_types.h"
#include "DNA_vfont_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_world_types.h"

#include "BLI_blenlib.h"
#include "BLI_utildefines.h"

#include "BLI_threads.h"
#include "BLT_translation.h"

#include "RNA_access.h"
#include "RNA_types.h"

#include "BKE_action.h"
#include "BKE_animsys.h"
#include "BKE_armature.h"
#include "BKE_asset.h"
#include "BKE_bpath.h"
#include "BKE_brush.h"
#include "BKE_camera.h"
#include "BKE_context.h"
#include "BKE_curve.h"
#include "BKE_depsgraph.h"
#include "BKE_fcurve.h"
#include "BKE_font.h"
#include "BKE_global.h"
#include "BKE_group.h"
#include "BKE_gpencil.h"
#include "BKE_idcode.h"
#include "BKE_idprop.h"
#include "BKE_image.h"
#include "BKE_ipo.h"
#include "BKE_key.h"
#include "BKE_lamp.h"
#include "BKE_lattice.h"
#include "BKE_library.h"
#include "BKE_library_query.h"
#include "BKE_linestyle.h"
#include "BKE_mesh.h"
#include "BKE_material.h"
#include "BKE_main.h"
#include "BKE_mball.h"
#include "BKE_movieclip.h"
#include "BKE_mask.h"
#include "BKE_node.h"
#include "BKE_object.h"
#include "BKE_paint.h"
#include "BKE_particle.h"
#include "BKE_packedFile.h"
#include "BKE_speaker.h"
#include "BKE_sound.h"
#include "BKE_screen.h"
#include "BKE_scene.h"
#include "BKE_text.h"
#include "BKE_texture.h"
#include "BKE_world.h"

#include "DEG_depsgraph.h"

#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"

#ifdef WITH_PYTHON
#include "BPY_extern.h"
#endif

/* GS reads the memory pointed at in a specific ordering. 
 * only use this definition, makes little and big endian systems
 * work fine, in conjunction with MAKE_ID */

/* ************* general ************************ */


/* this has to be called from each make_local_* func, we could call
 * from id_make_local() but then the make local functions would not be self
 * contained.
 * also note that the id _must_ have a library - campbell */
void BKE_id_lib_local_paths(Main *bmain, Library *lib, ID *id)
{
	const char *bpath_user_data[2] = {bmain->name, lib->filepath};

	BKE_bpath_traverse_id(bmain, id,
	                      BKE_bpath_relocate_visitor,
	                      BKE_BPATH_TRAVERSE_SKIP_MULTIFILE,
	                      (void *)bpath_user_data);
}

void id_lib_extern(ID *id)
{
	if (id) {
		BLI_assert(BKE_idcode_is_linkable(GS(id->name)));
		if (id->tag & LIB_TAG_INDIRECT) {
			id->tag -= LIB_TAG_INDIRECT;
			id->tag |= LIB_TAG_EXTERN;
		}
	}
}

/* ensure we have a real user */
/* Note: Now that we have flags, we could get rid of the 'fake_user' special case, flags are enough to ensure
 *       we always have a real user.
 *       However, ID_REAL_USERS is used in several places outside of core library.c, so think we can wait later
 *       to make this change... */
void id_us_ensure_real(ID *id)
{
	if (id) {
		const int limit = ID_FAKE_USERS(id);
		id->tag |= LIB_TAG_EXTRAUSER;
		if (id->us <= limit) {
			if (id->us < limit || ((id->us == limit) && (id->tag & LIB_TAG_EXTRAUSER_SET))) {
				printf("ID user count error: %s (from '%s')\n", id->name, id->lib ? id->lib->filepath : "[Main]");
				BLI_assert(0);
			}
			id->us = limit + 1;
			id->tag |= LIB_TAG_EXTRAUSER_SET;
		}
	}
}

void id_us_clear_real(ID *id)
{
	if (id && (id->tag & LIB_TAG_EXTRAUSER)) {
		if (id->tag & LIB_TAG_EXTRAUSER_SET) {
			id->us--;
			BLI_assert(id->us >= ID_FAKE_USERS(id));
		}
		id->tag &= ~(LIB_TAG_EXTRAUSER | LIB_TAG_EXTRAUSER_SET);
	}
}

/**
 * Same as \a id_us_plus, but does not handle lib indirect -> extern.
 * Only used by readfile.c so far, but simpler/safer to keep it here nonetheless.
 */
void id_us_plus_no_lib(ID *id)
{
	if (id) {
		if ((id->tag & LIB_TAG_EXTRAUSER) && (id->tag & LIB_TAG_EXTRAUSER_SET)) {
			BLI_assert(id->us >= 1);
			/* No need to increase count, just tag extra user as no more set.
			 * Avoids annoying & inconsistent +1 in user count. */
			id->tag &= ~LIB_TAG_EXTRAUSER_SET;
		}
		else {
			BLI_assert(id->us >= 0);
			id->us++;
		}
	}
}


void id_us_plus(ID *id)
{
	if (id) {
		id_us_plus_no_lib(id);
		id_lib_extern(id);
	}
}

/* decrements the user count for *id. */
void id_us_min(ID *id)
{
	if (id) {
		const int limit = ID_FAKE_USERS(id);

		if (id->us <= limit) {
			printf("ID user decrement error: %s (from '%s'): %d <= %d\n",
			       id->name, id->lib ? id->lib->filepath : "[Main]", id->us, limit);
			BLI_assert(0);
			id->us = limit;
		}
		else {
			id->us--;
		}

		if ((id->us == limit) && (id->tag & LIB_TAG_EXTRAUSER)) {
			/* We need an extra user here, but never actually incremented user count for it so far, do it now. */
			id_us_ensure_real(id);
		}
	}
}

void id_fake_user_set(ID *id)
{
	if (id && !(id->flag & LIB_FAKEUSER)) {
		id->flag |= LIB_FAKEUSER;
		id_us_plus(id);
	}
}

void id_fake_user_clear(ID *id)
{
	if (id && (id->flag & LIB_FAKEUSER)) {
		id->flag &= ~LIB_FAKEUSER;
		id_us_min(id);
	}
}

/* calls the appropriate make_local method for the block, unless test. Returns true
 * if the block can be made local. */
bool id_make_local(ID *id, bool test)
{
	if (id->tag & LIB_TAG_INDIRECT)
		return false;

	switch (GS(id->name)) {
		case ID_SCE:
			return false; /* not implemented */
		case ID_LI:
			return false; /* can't be linked */
		case ID_OB:
			if (!test) BKE_object_make_local((Object *)id);
			return true;
		case ID_ME:
			if (!test) {
				BKE_mesh_make_local((Mesh *)id);
				BKE_key_make_local(((Mesh *)id)->key);
			}
			return true;
		case ID_CU:
			if (!test) {
				BKE_curve_make_local((Curve *)id);
				BKE_key_make_local(((Curve *)id)->key);
			}
			return true;
		case ID_MB:
			if (!test) BKE_mball_make_local((MetaBall *)id);
			return true;
		case ID_MA:
			if (!test) BKE_material_make_local((Material *)id);
			return true;
		case ID_TE:
			if (!test) BKE_texture_make_local((Tex *)id);
			return true;
		case ID_IM:
			if (!test) BKE_image_make_local((Image *)id);
			return true;
		case ID_LT:
			if (!test) {
				BKE_lattice_make_local((Lattice *)id);
				BKE_key_make_local(((Lattice *)id)->key);
			}
			return true;
		case ID_LA:
			if (!test) BKE_lamp_make_local((Lamp *)id);
			return true;
		case ID_CA:
			if (!test) BKE_camera_make_local((Camera *)id);
			return true;
		case ID_SPK:
			if (!test) BKE_speaker_make_local((Speaker *)id);
			return true;
		case ID_IP:
			return false; /* deprecated */
		case ID_KE:
			if (!test) BKE_key_make_local((Key *)id);
			return true;
		case ID_WO:
			if (!test) BKE_world_make_local((World *)id);
			return true;
		case ID_SCR:
			return false; /* can't be linked */
		case ID_VF:
			return false; /* not implemented */
		case ID_TXT:
			return false; /* not implemented */
		case ID_SO:
			return false; /* not implemented */
		case ID_GR:
			return false; /* not implemented */
		case ID_AR:
			if (!test) BKE_armature_make_local((bArmature *)id);
			return true;
		case ID_AC:
			if (!test) BKE_action_make_local((bAction *)id);
			return true;
		case ID_NT:
			if (!test) ntreeMakeLocal((bNodeTree *)id, true);
			return true;
		case ID_BR:
			if (!test) BKE_brush_make_local((Brush *)id);
			return true;
		case ID_PA:
			if (!test) BKE_particlesettings_make_local((ParticleSettings *)id);
			return true;
		case ID_WM:
			return false; /* can't be linked */
		case ID_GD:
			return false; /* not implemented */
		case ID_LS:
			return false; /* not implemented */
	}

	return false;
}

/**
 * Invokes the appropriate copy method for the block and returns the result in
 * newid, unless test. Returns true if the block can be copied.
 */
bool id_copy(ID *id, ID **newid, bool test)
{
	if (!test) *newid = NULL;

	/* conventions:
	 * - make shallow copy, only this ID block
	 * - id.us of the new ID is set to 1 */
	switch (GS(id->name)) {
		case ID_SCE:
			return false;  /* can't be copied from here */
		case ID_LI:
			return false;  /* can't be copied from here */
		case ID_OB:
			if (!test) *newid = (ID *)BKE_object_copy((Object *)id);
			return true;
		case ID_ME:
			if (!test) *newid = (ID *)BKE_mesh_copy((Mesh *)id);
			return true;
		case ID_CU:
			if (!test) *newid = (ID *)BKE_curve_copy((Curve *)id);
			return true;
		case ID_MB:
			if (!test) *newid = (ID *)BKE_mball_copy((MetaBall *)id);
			return true;
		case ID_MA:
			if (!test) *newid = (ID *)BKE_material_copy((Material *)id);
			return true;
		case ID_TE:
			if (!test) *newid = (ID *)BKE_texture_copy((Tex *)id);
			return true;
		case ID_IM:
			if (!test) *newid = (ID *)BKE_image_copy(G.main, (Image *)id);
			return true;
		case ID_LT:
			if (!test) *newid = (ID *)BKE_lattice_copy((Lattice *)id);
			return true;
		case ID_LA:
			if (!test) *newid = (ID *)BKE_lamp_copy((Lamp *)id);
			return true;
		case ID_SPK:
			if (!test) *newid = (ID *)BKE_speaker_copy((Speaker *)id);
			return true;
		case ID_CA:
			if (!test) *newid = (ID *)BKE_camera_copy((Camera *)id);
			return true;
		case ID_IP:
			return false;  /* deprecated */
		case ID_KE:
			if (!test) *newid = (ID *)BKE_key_copy((Key *)id);
			return true;
		case ID_WO:
			if (!test) *newid = (ID *)BKE_world_copy((World *)id);
			return true;
		case ID_SCR:
			return false;  /* can't be copied from here */
		case ID_VF:
			return false;  /* not implemented */
		case ID_TXT:
			if (!test) *newid = (ID *)BKE_text_copy(G.main, (Text *)id);
			return true;
		case ID_SO:
			return false;  /* not implemented */
		case ID_GR:
			if (!test) *newid = (ID *)BKE_group_copy((Group *)id);
			return true;
		case ID_AR:
			if (!test) *newid = (ID *)BKE_armature_copy((bArmature *)id);
			return true;
		case ID_AC:
			if (!test) *newid = (ID *)BKE_action_copy((bAction *)id);
			return true;
		case ID_NT:
			if (!test) *newid = (ID *)ntreeCopyTree((bNodeTree *)id);
			return true;
		case ID_BR:
			if (!test) *newid = (ID *)BKE_brush_copy((Brush *)id);
			return true;
		case ID_PA:
			if (!test) *newid = (ID *)BKE_particlesettings_copy((ParticleSettings *)id);
			return true;
		case ID_WM:
			return false;  /* can't be copied from here */
		case ID_GD:
			if (!test) *newid = (ID *)gpencil_data_duplicate((bGPdata *)id, false);
			return true;
		case ID_MSK:
			if (!test) *newid = (ID *)BKE_mask_copy((Mask *)id);
			return true;
		case ID_LS:
			if (!test) *newid = (ID *)BKE_linestyle_copy(G.main, (FreestyleLineStyle *)id);
			return true;
	}
	
	return false;
}

bool id_single_user(bContext *C, ID *id, PointerRNA *ptr, PropertyRNA *prop)
{
	ID *newid = NULL;
	PointerRNA idptr;
	
	if (id) {
		/* if property isn't editable, we're going to have an extra block hanging around until we save */
		if (RNA_property_editable(ptr, prop)) {
			if (id_copy(id, &newid, false) && newid) {
				/* copy animation actions too */
				BKE_animdata_copy_id_action(id);
				/* us is 1 by convention, but RNA_property_pointer_set
				 * will also increment it, so set it to zero */
				newid->us = 0;
				
				/* assign copy */
				RNA_id_pointer_create(newid, &idptr);
				RNA_property_pointer_set(ptr, prop, idptr);
				RNA_property_update(C, ptr, prop);
				
				return true;
			}
		}
	}
	
	return false;
}

ListBase *which_libbase(Main *mainlib, short type)
{
	switch (type) {
		case ID_SCE:
			return &(mainlib->scene);
		case ID_LI:
			return &(mainlib->library);
		case ID_OB:
			return &(mainlib->object);
		case ID_ME:
			return &(mainlib->mesh);
		case ID_CU:
			return &(mainlib->curve);
		case ID_MB:
			return &(mainlib->mball);
		case ID_MA:
			return &(mainlib->mat);
		case ID_TE:
			return &(mainlib->tex);
		case ID_IM:
			return &(mainlib->image);
		case ID_LT:
			return &(mainlib->latt);
		case ID_LA:
			return &(mainlib->lamp);
		case ID_CA:
			return &(mainlib->camera);
		case ID_IP:
			return &(mainlib->ipo);
		case ID_KE:
			return &(mainlib->key);
		case ID_WO:
			return &(mainlib->world);
		case ID_SCR:
			return &(mainlib->screen);
		case ID_VF:
			return &(mainlib->vfont);
		case ID_TXT:
			return &(mainlib->text);
		case ID_SPK:
			return &(mainlib->speaker);
		case ID_SO:
			return &(mainlib->sound);
		case ID_GR:
			return &(mainlib->group);
		case ID_AR:
			return &(mainlib->armature);
		case ID_AC:
			return &(mainlib->action);
		case ID_NT:
			return &(mainlib->nodetree);
		case ID_BR:
			return &(mainlib->brush);
		case ID_PA:
			return &(mainlib->particle);
		case ID_WM:
			return &(mainlib->wm);
		case ID_GD:
			return &(mainlib->gpencil);
		case ID_MC:
			return &(mainlib->movieclip);
		case ID_MSK:
			return &(mainlib->mask);
		case ID_LS:
			return &(mainlib->linestyle);
		case ID_PAL:
			return &(mainlib->palettes);
		case ID_PC:
			return &(mainlib->paintcurves);
	}
	return NULL;
}

/**
 * Clear or set given tags for all ids in listbase (runtime tags).
 */
void BKE_main_id_tag_listbase(ListBase *lb, const int tag, const bool value)
{
	ID *id;
	if (value) {
		for (id = lb->first; id; id = id->next) {
			id->tag |= tag;
		}
	}
	else {
		const int ntag = ~tag;
		for (id = lb->first; id; id = id->next) {
			id->tag &= ntag;
		}
	}
}

/**
 * Clear or set given tags for all ids of given type in bmain (runtime tags).
 */
void BKE_main_id_tag_idcode(struct Main *mainvar, const short type, const int tag, const bool value)
{
	ListBase *lb = which_libbase(mainvar, type);

	BKE_main_id_tag_listbase(lb, tag, value);
}

/**
 * Clear or set given tags for all ids in bmain (runtime tags).
 */
void BKE_main_id_tag_all(struct Main *mainvar, const int tag, const bool value)
{
	ListBase *lbarray[MAX_LIBARRAY];
	int a;

	a = set_listbasepointers(mainvar, lbarray);
	while (a--) {
		BKE_main_id_tag_listbase(lbarray[a], tag, value);
	}
}


/**
 * Clear or set given flags for all ids in listbase (persistent flags).
 */
void BKE_main_id_flag_listbase(ListBase *lb, const int flag, const bool value)
{
	ID *id;
	if (value) {
		for (id = lb->first; id; id = id->next)
			id->tag |= flag;
	}
	else {
		const int nflag = ~flag;
		for (id = lb->first; id; id = id->next)
			id->tag &= nflag;
	}
}

/**
 * Clear or set given flags for all ids in bmain (persistent flags).
 */
void BKE_main_id_flag_all(Main *bmain, const int flag, const bool value)
{
	ListBase *lbarray[MAX_LIBARRAY];
	int a;
	a = set_listbasepointers(bmain, lbarray);
	while (a--) {
		BKE_main_id_flag_listbase(lbarray[a], flag, value);
	}
}

void BKE_main_lib_objects_recalc_all(Main *bmain)
{
	Object *ob;

	/* flag for full recalc */
	for (ob = bmain->object.first; ob; ob = ob->id.next) {
		if (ob->id.lib) {
			DAG_id_tag_update(&ob->id, OB_RECALC_OB | OB_RECALC_DATA | OB_RECALC_TIME);
		}
	}

	DAG_id_type_tag(bmain, ID_OB);
}

/**
 * puts into array *lb pointers to all the ListBase structs in main,
 * and returns the number of them as the function result. This is useful for
 * generic traversal of all the blocks in a Main (by traversing all the
 * lists in turn), without worrying about block types.
 *
 * \note MAX_LIBARRAY define should match this code */
int set_listbasepointers(Main *main, ListBase **lb)
{
	int a = 0;

	/* BACKWARDS! also watch order of free-ing! (mesh<->mat), first items freed last.
	 * This is important because freeing data decreases usercounts of other datablocks,
	 * if this data is its self freed it can crash. */
	lb[a++] = &(main->library);  /* Libraries may be accessed from pretty much any other ID... */
	lb[a++] = &(main->ipo);
	lb[a++] = &(main->action); /* moved here to avoid problems when freeing with animato (aligorith) */
	lb[a++] = &(main->key);
	lb[a++] = &(main->gpencil); /* referenced by nodes, objects, view, scene etc, before to free after. */
	lb[a++] = &(main->nodetree);
	lb[a++] = &(main->image);
	lb[a++] = &(main->tex);
	lb[a++] = &(main->mat);
	lb[a++] = &(main->vfont);
	
	/* Important!: When adding a new object type,
	 * the specific data should be inserted here 
	 */

	lb[a++] = &(main->armature);

	lb[a++] = &(main->mesh);
	lb[a++] = &(main->curve);
	lb[a++] = &(main->mball);

	lb[a++] = &(main->latt);
	lb[a++] = &(main->lamp);
	lb[a++] = &(main->camera);

	lb[a++] = &(main->text);
	lb[a++] = &(main->sound);
	lb[a++] = &(main->group);
	lb[a++] = &(main->palettes);
	lb[a++] = &(main->paintcurves);
	lb[a++] = &(main->brush);
	lb[a++] = &(main->particle);
	lb[a++] = &(main->speaker);

	lb[a++] = &(main->world);
	lb[a++] = &(main->movieclip);
	lb[a++] = &(main->screen);
	lb[a++] = &(main->object);
	lb[a++] = &(main->linestyle); /* referenced by scenes */
	lb[a++] = &(main->scene);
	lb[a++] = &(main->wm);
	lb[a++] = &(main->mask);
	
	lb[a] = NULL;

	BLI_assert(a + 1 == MAX_LIBARRAY);

	return a;
}

/* *********** ALLOC AND FREE *****************
 *
 * BKE_libblock_free(ListBase *lb, ID *id )
 * provide a list-basis and datablock, but only ID is read
 *
 * void *BKE_libblock_alloc(ListBase *lb, type, name)
 * inserts in list and returns a new ID
 *
 * **************************** */

/**
 * Allocates and returns memory of the right size for the specified block type,
 * initialized to zero.
 */
void *BKE_libblock_alloc_notest(short type)
{
	ID *id = NULL;
	
	switch (type) {
		case ID_SCE:
			id = MEM_callocN(sizeof(Scene), "scene");
			break;
		case ID_LI:
			id = MEM_callocN(sizeof(Library), "library");
			break;
		case ID_OB:
			id = MEM_callocN(sizeof(Object), "object");
			break;
		case ID_ME:
			id = MEM_callocN(sizeof(Mesh), "mesh");
			break;
		case ID_CU:
			id = MEM_callocN(sizeof(Curve), "curve");
			break;
		case ID_MB:
			id = MEM_callocN(sizeof(MetaBall), "mball");
			break;
		case ID_MA:
			id = MEM_callocN(sizeof(Material), "mat");
			break;
		case ID_TE:
			id = MEM_callocN(sizeof(Tex), "tex");
			break;
		case ID_IM:
			id = MEM_callocN(sizeof(Image), "image");
			break;
		case ID_LT:
			id = MEM_callocN(sizeof(Lattice), "latt");
			break;
		case ID_LA:
			id = MEM_callocN(sizeof(Lamp), "lamp");
			break;
		case ID_CA:
			id = MEM_callocN(sizeof(Camera), "camera");
			break;
		case ID_IP:
			id = MEM_callocN(sizeof(Ipo), "ipo");
			break;
		case ID_KE:
			id = MEM_callocN(sizeof(Key), "key");
			break;
		case ID_WO:
			id = MEM_callocN(sizeof(World), "world");
			break;
		case ID_SCR:
			id = MEM_callocN(sizeof(bScreen), "screen");
			break;
		case ID_VF:
			id = MEM_callocN(sizeof(VFont), "vfont");
			break;
		case ID_TXT:
			id = MEM_callocN(sizeof(Text), "text");
			break;
		case ID_SPK:
			id = MEM_callocN(sizeof(Speaker), "speaker");
			break;
		case ID_SO:
			id = MEM_callocN(sizeof(bSound), "sound");
			break;
		case ID_GR:
			id = MEM_callocN(sizeof(Group), "group");
			break;
		case ID_AR:
			id = MEM_callocN(sizeof(bArmature), "armature");
			break;
		case ID_AC:
			id = MEM_callocN(sizeof(bAction), "action");
			break;
		case ID_NT:
			id = MEM_callocN(sizeof(bNodeTree), "nodetree");
			break;
		case ID_BR:
			id = MEM_callocN(sizeof(Brush), "brush");
			break;
		case ID_PA:
			id = MEM_callocN(sizeof(ParticleSettings), "ParticleSettings");
			break;
		case ID_WM:
			id = MEM_callocN(sizeof(wmWindowManager), "Window manager");
			break;
		case ID_GD:
			id = MEM_callocN(sizeof(bGPdata), "Grease Pencil");
			break;
		case ID_MC:
			id = MEM_callocN(sizeof(MovieClip), "Movie Clip");
			break;
		case ID_MSK:
			id = MEM_callocN(sizeof(Mask), "Mask");
			break;
		case ID_LS:
			id = MEM_callocN(sizeof(FreestyleLineStyle), "Freestyle Line Style");
			break;
		case ID_PAL:
			id = MEM_callocN(sizeof(Palette), "Palette");
			break;
		case ID_PC:
			id = MEM_callocN(sizeof(PaintCurve), "Paint Curve");
			break;
	}
	return id;
}

/**
 * Allocates and returns a block of the specified type, with the specified name
 * (adjusted as necessary to ensure uniqueness), and appended to the specified list.
 * The user count is set to 1, all other content (apart from name and links) being
 * initialized to zero.
 */
void *BKE_libblock_alloc(Main *bmain, short type, const char *name)
{
	ID *id = NULL;
	ListBase *lb = which_libbase(bmain, type);
	
	id = BKE_libblock_alloc_notest(type);
	if (id) {
		BKE_main_lock(bmain);
		BLI_addtail(lb, id);
		id->us = 1;
		id->icon_id = 0;
		*( (short *)id->name) = type;
		new_id(lb, id, name);
		/* alphabetic insertion: is in new_id */
		BKE_main_unlock(bmain);
	}
	DAG_id_type_tag(bmain, type);
	return id;
}

/**
 * Initialize an ID of given type, such that it has valid 'empty' data.
 * ID is assumed to be just calloc'ed.
 */
void BKE_libblock_init_empty(ID *id)
{
	/* Note that only ID types that are not valid when filled of zero should have a callback here. */
	switch (GS(id->name)) {
		case ID_SCE:
			BKE_scene_init((Scene *)id);
			break;
		case ID_LI:
			/* Nothing to do. */
			break;
		case ID_OB:
		{
			Object *ob = (Object *)id;
			ob->type = OB_EMPTY;
			BKE_object_init(ob);
			break;
		}
		case ID_ME:
			BKE_mesh_init((Mesh *)id);
			break;
		case ID_CU:
			BKE_curve_init((Curve *)id);
			break;
		case ID_MB:
			BKE_mball_init((MetaBall *)id);
			break;
		case ID_MA:
			BKE_material_init((Material *)id);
			break;
		case ID_TE:
			BKE_texture_default((Tex *)id);
			break;
		case ID_IM:
			BKE_image_init((Image *)id);
			break;
		case ID_LT:
			BKE_lattice_init((Lattice *)id);
			break;
		case ID_LA:
			BKE_lamp_init((Lamp *)id);
			break;
		case ID_SPK:
			BKE_speaker_init((Speaker *)id);
			break;
		case ID_CA:
			BKE_camera_init((Camera *)id);
			break;
		case ID_IP:
			/* Should not be needed - animation from lib pre-2.5 is broken anyway. */
			BLI_assert(0);
			break;
		case ID_KE:
			/* Shapekeys are a complex topic too - they depend on their 'user' data type...
			 * They are not linkable, though, so it should never reach here anyway. */
			BLI_assert(0);
			break;
		case ID_WO:
			BKE_world_init((World *)id);
			break;
		case ID_SCR:
			/* Nothing to do. */
			break;
		case ID_VF:
			BKE_vfont_init((VFont *)id);
			break;
		case ID_TXT:
			BKE_text_init((Text *)id);
			break;
		case ID_SO:
			/* Another fuzzy case, think NULLified content is OK here... */
			break;
		case ID_GR:
			/* Nothing to do. */
			break;
		case ID_AR:
			/* Nothing to do. */
			break;
		case ID_AC:
			/* Nothing to do. */
			break;
		case ID_NT:
			ntreeInitDefault((bNodeTree *)id);
			break;
		case ID_BR:
			BKE_brush_init((Brush *)id);
			break;
		case ID_PA:
			/* Nothing to do. */
			break;
		case ID_PC:
			/* Nothing to do. */
			break;
		case ID_WM:
			/* We should never reach this. */
			BLI_assert(0);
			break;
		case ID_GD:
			/* Nothing to do. */
			break;
		case ID_MSK:
			/* Nothing to do. */
			break;
		case ID_LS:
			BKE_linestyle_init((FreestyleLineStyle *)id);
			break;
	}
}

/* by spec, animdata is first item after ID */
/* and, trust that BKE_animdata_from_id() will only find AnimData for valid ID-types */
static void id_copy_animdata(ID *id, const bool do_action)
{
	AnimData *adt = BKE_animdata_from_id(id);
	
	if (adt) {
		IdAdtTemplate *iat = (IdAdtTemplate *)id;
		iat->adt = BKE_animdata_copy(iat->adt, do_action); /* could be set to false, need to investigate */
	}
}

/* material nodes use this since they are not treated as libdata */
void BKE_libblock_copy_data(ID *id, const ID *id_from, const bool do_action)
{
	if (id_from->properties)
		id->properties = IDP_CopyProperty(id_from->properties);

	/* the duplicate should get a copy of the animdata */
	id_copy_animdata(id, do_action);
}

/* used everywhere in blenkernel */
void *BKE_libblock_copy_ex(Main *bmain, ID *id)
{
	ID *idn;
	size_t idn_len;

	idn = BKE_libblock_alloc(bmain, GS(id->name), id->name + 2);

	assert(idn != NULL);

	idn_len = MEM_allocN_len(idn);
	if ((int)idn_len - (int)sizeof(ID) > 0) { /* signed to allow neg result */
		const char *cp = (const char *)id;
		char *cpn = (char *)idn;

		memcpy(cpn + sizeof(ID), cp + sizeof(ID), idn_len - sizeof(ID));
	}
	
	id->newid = idn;
	idn->tag |= LIB_TAG_NEW;

	BKE_libblock_copy_data(idn, id, false);
	
	return idn;
}

void *BKE_libblock_copy_nolib(ID *id, const bool do_action)
{
	ID *idn;
	size_t idn_len;

	idn = BKE_libblock_alloc_notest(GS(id->name));
	assert(idn != NULL);

	BLI_strncpy(idn->name, id->name, sizeof(idn->name));

	idn_len = MEM_allocN_len(idn);
	if ((int)idn_len - (int)sizeof(ID) > 0) { /* signed to allow neg result */
		const char *cp = (const char *)id;
		char *cpn = (char *)idn;

		memcpy(cpn + sizeof(ID), cp + sizeof(ID), idn_len - sizeof(ID));
	}

	id->newid = idn;
	idn->tag |= LIB_TAG_NEW;
	idn->us = 1;

	BKE_libblock_copy_data(idn, id, do_action);

	return idn;
}

void *BKE_libblock_copy(ID *id)
{
	return BKE_libblock_copy_ex(G.main, id);
}

static int id_relink_looper(void *UNUSED(user_data), ID *UNUSED(self_id), ID **id_pointer, const int cd_flag)
{
	ID *id = *id_pointer;
	if (id) {
		/* See: NEW_ID macro */
		if (id->newid) {
			BKE_library_update_ID_link_user(id->newid, id, cd_flag);
			*id_pointer = id->newid;
		}
		else if (id->tag & LIB_TAG_NEW) {
			id->tag &= ~LIB_TAG_NEW;
			BKE_libblock_relink(id);
		}
	}
	return IDWALK_RET_NOP;
}

void BKE_libblock_relink(ID *id)
{
	if (id->lib)
		return;

	BKE_library_foreach_ID_link(id, id_relink_looper, NULL, 0);
}

static void BKE_library_free(Library *lib)
{
	if (lib->packedfile)
		freePackedFile(lib->packedfile);

	BKE_library_asset_repository_free(lib);
}

static BKE_library_free_window_manager_cb free_windowmanager_cb = NULL;

void BKE_library_callback_free_window_manager_set(BKE_library_free_window_manager_cb func)
{
	free_windowmanager_cb = func;
}

static BKE_library_free_notifier_reference_cb free_notifier_reference_cb = NULL;

void BKE_library_callback_free_notifier_reference_set(BKE_library_free_notifier_reference_cb func)
{
	free_notifier_reference_cb = func;
}

static BKE_library_remap_editor_id_reference_cb remap_editor_id_reference_cb = NULL;

void BKE_library_callback_remap_editor_id_reference_set(BKE_library_remap_editor_id_reference_cb func)
{
	remap_editor_id_reference_cb = func;
}

typedef struct IDRemap {
	ID *old_id;
	ID *new_id;
	ID *id;  /* The ID in which we are replacing old_id by new_id usages. */
	short flag;

	/* 'Output' data. */
	short status;
	int skipped_direct;  /* Number of direct usecases that could not be remapped (e.g.: obdata when in edit mode). */
	int skipped_indirect;  /* Number of indirect usecases that could not be remapped. */
	int skipped_refcounted;  /* Number of skipped usecases that refcount the datablock. */
} IDRemap;

/* IDRemap->flag enums defined in BKE_library.h */

/* IDRemap->status */
enum {
	/* *** Set by callback. *** */
	ID_REMAP_IS_LINKED_DIRECT       = 1 << 0,  /* new_id is directly linked in current .blend. */
	ID_REMAP_IS_USER_ONE_SKIPPED    = 1 << 1,  /* There was some skipped 'user_one' usages of old_id. */
};

static int foreach_libblock_remap_callback(void *user_data, ID *UNUSED(id_self), ID **id_p, int cb_flag)
{
	IDRemap *id_remap_data = user_data;
	ID *old_id = id_remap_data->old_id;
	ID *new_id = id_remap_data->new_id;
	ID *id = id_remap_data->id;

	if (!old_id) {  /* Used to cleanup all IDs used by a specific one. */
		BLI_assert(!new_id);
		old_id = *id_p;
	}

	if (*id_p && (*id_p == old_id)) {
		/* Note: proxy usage implies LIB_TAG_EXTERN, so on this aspect it is direct,
		 *       on the other hand since they get reset to lib data on file open/reload it is indirect too...
		 *       Edit Mode is also a 'skip direct' case. */
		const bool is_obj = (GS(id->name) == ID_OB);
		const bool is_proxy = (is_obj && (((Object *)id)->proxy || ((Object *)id)->proxy_group));
		const bool is_obj_editmode = (is_obj && BKE_object_is_in_editmode((Object *)id));
		/* Note that indirect data from same file as processed ID is **not** considered indirect! */
		const bool is_indirect = ((id->lib != NULL) && (id->lib != old_id->lib));
		const bool skip_indirect = (id_remap_data->flag & ID_REMAP_SKIP_INDIRECT_USAGE) != 0;
		const bool is_never_null = ((cb_flag & IDWALK_NEVER_NULL) && (new_id == NULL) &&
		                            (id_remap_data->flag & ID_REMAP_FORCE_NEVER_NULL_USAGE) == 0);
		const bool skip_never_null = (id_remap_data->flag & ID_REMAP_SKIP_NEVER_NULL_USAGE) != 0;

		if ((id_remap_data->flag & ID_REMAP_FLAG_NEVER_NULL_USAGE) && (cb_flag & IDWALK_NEVER_NULL)) {
			id->tag |= LIB_TAG_DOIT;
		}

//		if (GS(old_id->name) == ID_TXT) {
//			printf("\t\t %s (from %s) (%d)\n", old_id->name, old_id->lib ? old_id->lib->filepath : "<MAIN>", old_id->us);
//			printf("\t\tIn %s (%p): remapping %s (%p) to %s (%p)\n",
//			       id->name, id, old_id->name, old_id, new_id ? new_id->name : "<NONE>", new_id);
//		}

		/* Special hack in case it's Object->data and we are in edit mode (skipped_direct too). */
		if ((is_never_null && skip_never_null) ||
		    (is_obj_editmode && (((Object *)id)->data == *id_p)) ||
		    (skip_indirect && (is_proxy || is_indirect)))
		{
			if (is_never_null || is_proxy || is_obj_editmode) {
				id_remap_data->skipped_direct++;
			}
			else {
				id_remap_data->skipped_indirect++;
			}
			if (cb_flag & IDWALK_USER) {
				id_remap_data->skipped_refcounted++;
			}
			else if (cb_flag & IDWALK_USER_ONE) {
				/* No need to count number of times this happens, just a flag is enough. */
				id_remap_data->status |= ID_REMAP_IS_USER_ONE_SKIPPED;
			}
		}
		else {
			if (!is_never_null) {
				*id_p = new_id;
			}
			if (cb_flag & IDWALK_USER) {
				id_us_min(old_id);
				/* We do not want to handle LIB_TAG_INDIRECT/LIB_TAG_EXTERN here. */
				if (new_id)
					new_id->us++;
			}
			else if (cb_flag & IDWALK_USER_ONE) {
				id_us_ensure_real(new_id);
				/* We cannot affect old_id->us directly, LIB_TAG_EXTRAUSER(_SET) are assumed to be set as needed,
				 * that extra user is processed in final handling... */
			}
			if (!is_indirect) {
				id_remap_data->status |= ID_REMAP_IS_LINKED_DIRECT;
			}
		}
	}

	return IDWALK_RET_NOP;
}

/**
 * Execute the 'data' part of the remapping (that is, all ID pointers from other ID datablocks).
 *
 * Behavior differs depending on whether given \a id is NULL or not:
 *   - \a id NULL: \a old_id must be non-NULL, \a new_id may be NULL (unlinking \a old_id) or not
 *     (remapping \a old_id to \a new_id). The whole \a bmain database is checked, and all pointers to \a old_id
 *     are remapped to \a new_id.
 *   - \a id is non-NULL:
 *     + If \a old_id is NULL, \a new_id must also be NULL, and all ID pointers from \a id are cleared (i.e. \a id
 *       does not references any other datablock anymore).
 *     + If \a old_id is non-NULL, behavior is as with a NULL \a id, but only for given \a id.
 *
 * \param bmain the Main data storage to operate on (can be NULL if \a id is non-NULL).
 * \param id the datablock to operate on (can be NULL if \a bmain is non-NULL).
 * \param old_id the datablock to dereference (may be NULL if \a id is non-NULL).
 * \param new_id the new datablock to replace \a old_id references with (may be NULL).
 * \param skip_indirect_usage if true, do not remap/unlink indirect usages of \a old_id datablock.
 * \param r_id_remap_data if non-NULL, the IDRemap struct to use (uselful to retrieve info about remapping process).
 * \return true is there was some 'user_one' users of \a old_id (needed to handle correctly #old_id->us count).
 */
static void libblock_remap_data(
        Main *bmain, ID *id, ID *old_id, ID *new_id, const short remap_flags, IDRemap *r_id_remap_data)
{
	IDRemap id_remap_data;
	ListBase *lb_array[MAX_LIBARRAY];
	int i;

	if (r_id_remap_data == NULL) {
		r_id_remap_data = &id_remap_data;
	}
	r_id_remap_data->old_id = old_id;
	r_id_remap_data->new_id = new_id;
	r_id_remap_data->id = NULL;
	r_id_remap_data->flag = remap_flags;
	r_id_remap_data->status = 0;
	r_id_remap_data->skipped_direct = 0;
	r_id_remap_data->skipped_indirect = 0;
	r_id_remap_data->skipped_refcounted = 0;

//	if (old_id && GS(old_id->name) == ID_AC)
//		printf("%s: %s (%p) replaced by %s (%p)\n", __func__,
//			   old_id ? old_id->name : "", old_id, new_id ? new_id->name : "", new_id);

	if (id) {
//		printf("\tchecking id %s (%p, %p)\n", id->name, id, id->lib);
		r_id_remap_data->id = id;
		BKE_library_foreach_ID_link(id, foreach_libblock_remap_callback, (void *)r_id_remap_data, IDWALK_NOP);
	}
	else {
		i = set_listbasepointers(bmain, lb_array);

		/* Note that this is a very 'bruteforce' approach, maybe we could use some depsgraph to only process
		 * objects actually using given old_id... sounds rather unlikely currently, though, so this will do for now. */

		while (i--) {
			ID *id_curr = lb_array[i]->first;

			for (; id_curr; id_curr = id_curr->next) {
				/* Note that we cannot skip indirect usages of old_id here (if requested), we still need to check it for
				 * the user count handling...
				 * XXX No more true (except for debug usage of those skipping counters). */
//				if (GS(old_id->name) == ID_AC && STRCASEEQ(id_curr->name, "OBfranck_blenrig"))
//					printf("\tchecking id %s (%p, %p)\n", id_curr->name, id_curr, id_curr->lib);
				r_id_remap_data->id = id_curr;
				BKE_library_foreach_ID_link(
				            id_curr, foreach_libblock_remap_callback, (void *)r_id_remap_data, IDWALK_NOP);
			}
		}
	}

	/* XXX We may not want to always 'transfer' fakeuser from old to new id... Think for now it's desired behavior
	 *     though, we can always add an option (flag) to control this later if needed. */
	if (old_id && (old_id->flag & LIB_FAKEUSER)) {
		id_fake_user_clear(old_id);
		id_fake_user_set(new_id);
	}

	id_us_clear_real(old_id);

	if (new_id && (new_id->tag & LIB_TAG_INDIRECT) && (r_id_remap_data->status & ID_REMAP_IS_LINKED_DIRECT)) {
		new_id->tag &= ~LIB_TAG_INDIRECT;
		new_id->tag |= LIB_TAG_EXTERN;
	}

//	printf("%s: %d occurences skipped (%d direct and %d indirect ones)\n", __func__,
//	       r_id_remap_data->skipped_direct + r_id_remap_data->skipped_indirect,
//	       r_id_remap_data->skipped_direct, r_id_remap_data->skipped_indirect);
}

/**
 * Replace all references in given Main to \a old_id by \a new_id (if \a new_id is NULL, it unlinks \a old_id).
 *
 * \param skip_indirect_usage If \a true, indirect usages (like e.g. by other linked datablocks) are not remapped.
 * \param do_flag_never_null If \a true, 'NEVER_NULL' ID users are flagged with LIB_TAG_DOIT (caller is expected
 *                           to ensure that flag is correctly unset first).
 */
void BKE_libblock_remap_locked(
        Main *bmain, void *old_idv, void *new_idv,
        const short remap_flags)
{
	IDRemap id_remap_data;
	ID *old_id = old_idv;
	ID *new_id = new_idv;
	int skipped_direct, skipped_refcounted;

	BLI_assert(old_id != NULL);
	BLI_assert((new_id == NULL) || GS(old_id->name) == GS(new_id->name));
	BLI_assert(old_id != new_id);

	/* Some pre-process updates.
	 * This is a bit ugly, but cannot see a way to avoid it. Maybe we should do a per-ID callback for this instead?
	 */
	if (GS(old_id->name) == ID_OB) {
		Object *old_ob = (Object *)old_id;
		Object *new_ob = (Object *)new_id;

		if (new_ob == NULL) {
			Scene *sce;
			Base *base;

			for (sce = bmain->scene.first; sce; sce = sce->id.next) {
				base = BKE_scene_base_find(sce, old_ob);

				if (base) {
					id_us_min((ID *)base->object);
					BKE_scene_base_unlink(sce, base);
					MEM_freeN(base);
				}
			}
		}
	}

//	if (GS(old_id->name) == ID_AC) {
//		printf("%s: START %s (%p, %d) replaced by %s (%p, %d)\n",
//		       __func__, old_id->name, old_id, old_id->us, new_id ? new_id->name : "", new_id, new_id ? new_id->us : 0);
//	}

	libblock_remap_data(bmain, NULL, old_id, new_id, remap_flags, &id_remap_data);

	if (free_notifier_reference_cb) {
		free_notifier_reference_cb(old_id);
	}

	/* We assume editors do not hold references to their IDs... This is false in some cases
	 * (Image is especially tricky here), editors' code is to handle refcount (id->us) itself then. */
	if (remap_editor_id_reference_cb) {
		remap_editor_id_reference_cb(old_id, new_id);
	}

	skipped_direct = id_remap_data.skipped_direct;
	skipped_refcounted = id_remap_data.skipped_refcounted;

	/* If old_id was used by some ugly 'user_one' stuff (like Image or Clip editors...), and user count has actually
	 * been incremented for that, we have to decrease once more its user count... unless we had to skip
	 * some 'user_one' cases. */
	if ((old_id->tag & LIB_TAG_EXTRAUSER_SET) && !(id_remap_data.status & ID_REMAP_IS_USER_ONE_SKIPPED)) {
		id_us_min(old_id);
		old_id->tag &= ~LIB_TAG_EXTRAUSER_SET;
	}

	BLI_assert(old_id->us - skipped_refcounted >= 0);
	UNUSED_VARS_NDEBUG(skipped_refcounted);

	if (skipped_direct == 0) {
		/* old_id is assumed to not be used directly anymore... */
		if (old_id->lib && (old_id->tag & LIB_TAG_EXTERN)) {
			old_id->tag &= ~LIB_TAG_EXTERN;
			old_id->tag |= LIB_TAG_INDIRECT;
		}
	}

	/* Some after-process updates.
	 * This is a bit ugly, but cannot see a way to avoid it. Maybe we should do a per-ID callback for this instead?
	 */
	if (GS(old_id->name) == ID_OB) {
		Object *old_ob = (Object *)old_id;
		Object *new_ob = (Object *)new_id;

		if (old_ob->flag & OB_FROMGROUP) {
			/* Note that for Scene's BaseObject->flag, either we:
			 *     - unlinked old_ob (i.e. new_ob is NULL), in which case scenes' bases have been removed already.
			 *     - remaped old_ob by new_ob, in which case scenes' bases are still valid as is.
			 * So in any case, no need to update them here. */
			if (BKE_group_object_find(NULL, old_ob) == NULL) {
				old_ob->flag &= ~OB_FROMGROUP;
			}
			if (new_ob == NULL) {  /* We need to remove NULL-ified groupobjects... */
				Group *group;
				for (group = bmain->group.first; group; group = group->id.next) {
					BKE_group_object_unlink(group, NULL, NULL, NULL);
				}
			}
			else {
				new_ob->flag |= OB_FROMGROUP;
			}
		}
	}

//	if (GS(old_id->name) == ID_AC) {
//		printf("%s: END   %s (%p, %d) replaced by %s (%p, %d)\n",
//		       __func__, old_id->name, old_id, old_id->us, new_id ? new_id->name : "", new_id, new_id ? new_id->us : 0);
//	}

	/* Full rebuild of DAG! */
	DAG_relations_tag_update(bmain);
}

void BKE_libblock_remap(Main *bmain, void *old_idv, void *new_idv, const short remap_flags)
{
	BKE_main_lock(bmain);

	BKE_libblock_remap_locked(bmain, old_idv, new_idv, remap_flags);

	BKE_main_unlock(bmain);
}

/**
 * Unlink given \a id from given \a bmain (does not touch to indirect, i.e. library, usages of the ID).
 *
 * \param do_flag_never_null If true, all IDs using \a idv in a 'non-NULL' way are flagged by \a LIB_TAG_DOIT flag
 *                           (quite obviously, 'non-NULL' usages can never be unlinked by this function...).
 */
void BKE_libblock_unlink(Main *bmain, void *idv, const bool do_flag_never_null)
{
	const short remap_flags = ID_REMAP_SKIP_INDIRECT_USAGE | (do_flag_never_null ? ID_REMAP_FLAG_NEVER_NULL_USAGE : 0);

	BKE_main_lock(bmain);

	BKE_libblock_remap_locked(bmain, idv, NULL, remap_flags);

	BKE_main_unlock(bmain);
}

/** Similar to libblock_remap, but only affects IDs used by given \a idv ID.
 *
 * \param old_id Unlike BKE_libblock_remap, can be NULL, in which case all ID usages by given \a idv will be cleared.
 * \param us_min_never_null If \a true and new_id is NULL, 'NEVER_NULL' ID usages keep their old id, but this one still
 *        gets its user count decremented (needed when given \a idv is going to be deleted right after being unlinked).
 */
/* Should be able to replace all _relink() funcs (constraints, rigidbody, etc.) ? */
/* XXX Arg! Naming... :(
 *     _relink? avoids confusion with _remap, but is confusing with _unlink
 *     _remap_used_ids?
 *     _remap_datablocks?
 *     BKE_id_remap maybe?
 *     ... sigh
 */
void BKE_libblock_relink_ex(
        void *idv, void *old_idv, void *new_idv, const bool us_min_never_null)
{
	ID *id = idv;
	ID *old_id = old_idv;
	ID *new_id = new_idv;
	int remap_flags = us_min_never_null ? 0 : ID_REMAP_SKIP_NEVER_NULL_USAGE;

	/* No need to lock here, we are only affecting given ID. */

	BLI_assert(id);
	if (old_id) {
		BLI_assert((new_id == NULL) || GS(old_id->name) == GS(new_id->name));
		BLI_assert(old_id != new_id);
	}
	else {
		BLI_assert(new_id == NULL);
	}

	libblock_remap_data(NULL, id, old_id, new_id, remap_flags, NULL);
}

static void animdata_dtar_clear_cb(ID *UNUSED(id), AnimData *adt, void *userdata)
{
	ChannelDriver *driver;
	FCurve *fcu;

	/* find the driver this belongs to and update it */
	for (fcu = adt->drivers.first; fcu; fcu = fcu->next) {
		driver = fcu->driver;
		
		if (driver) {
			DriverVar *dvar;
			for (dvar = driver->variables.first; dvar; dvar = dvar->next) {
				DRIVER_TARGETS_USED_LOOPER(dvar) 
				{
					if (dtar->id == userdata)
						dtar->id = NULL;
				}
				DRIVER_TARGETS_LOOPER_END
			}
		}
	}
}

void BKE_libblock_free_data(Main *bmain, ID *id)
{
	if (id->properties) {
		IDP_FreeProperty(id->properties);
		MEM_freeN(id->properties);
	}

	MEM_SAFE_FREE(id->uuid);
	
	/* this ID may be a driver target! */
	BKE_animdata_main_cb(bmain, animdata_dtar_clear_cb, (void *)id);
}

/**
 * used in headerbuttons.c image.c mesh.c screen.c sound.c and library.c
 *
 * \param do_id_user if \a true, try to release other ID's 'references' hold by \a idv.
 */
void BKE_libblock_free_ex(Main *bmain, void *idv, const bool do_id_user)
{
	ID *id = idv;
	short type = GS(id->name);
	ListBase *lb = which_libbase(bmain, type);

	DAG_id_type_tag(bmain, type);

#ifdef WITH_PYTHON
	BPY_id_release(id);
#endif

	if (do_id_user) {
		BKE_libblock_relink_ex(id, NULL, NULL, true);
	}

	switch (type) {
		case ID_SCE:
			BKE_scene_free((Scene *)id);
			break;
		case ID_LI:
			BKE_library_free((Library *)id);
			break;
		case ID_OB:
			BKE_object_free((Object *)id);
			break;
		case ID_ME:
			BKE_mesh_free((Mesh *)id);
			break;
		case ID_CU:
			BKE_curve_free((Curve *)id);
			break;
		case ID_MB:
			BKE_mball_free((MetaBall *)id);
			break;
		case ID_MA:
			BKE_material_free((Material *)id);
			break;
		case ID_TE:
			BKE_texture_free((Tex *)id);
			break;
		case ID_IM:
			BKE_image_free((Image *)id);
			break;
		case ID_LT:
			BKE_lattice_free((Lattice *)id);
			break;
		case ID_LA:
			BKE_lamp_free((Lamp *)id);
			break;
		case ID_CA:
			BKE_camera_free((Camera *) id);
			break;
		case ID_IP:  /* Deprecated. */
			BKE_ipo_free((Ipo *)id);
			break;
		case ID_KE:
			BKE_key_free((Key *)id);
			break;
		case ID_WO:
			BKE_world_free((World *)id);
			break;
		case ID_SCR:
			BKE_screen_free((bScreen *)id);
			break;
		case ID_VF:
			BKE_vfont_free((VFont *)id);
			break;
		case ID_TXT:
			BKE_text_free((Text *)id);
			break;
		case ID_SPK:
			BKE_speaker_free((Speaker *)id);
			break;
		case ID_SO:
			BKE_sound_free((bSound *)id);
			break;
		case ID_GR:
			BKE_group_free((Group *)id);
			break;
		case ID_AR:
			BKE_armature_free((bArmature *)id);
			break;
		case ID_AC:
			BKE_action_free((bAction *)id);
			break;
		case ID_NT:
			ntreeFreeTree((bNodeTree *)id);
			break;
		case ID_BR:
			BKE_brush_free((Brush *)id);
			break;
		case ID_PA:
			BKE_particlesettings_free((ParticleSettings *)id);
			break;
		case ID_WM:
			if (free_windowmanager_cb)
				free_windowmanager_cb(NULL, (wmWindowManager *)id);
			break;
		case ID_GD:
			BKE_gpencil_free((bGPdata *)id);
			break;
		case ID_MC:
			BKE_movieclip_free((MovieClip *)id);
			break;
		case ID_MSK:
			BKE_mask_free((Mask *)id);
			break;
		case ID_LS:
			BKE_linestyle_free((FreestyleLineStyle *)id);
			break;
		case ID_PAL:
			BKE_palette_free((Palette *)id);
			break;
		case ID_PC:
			BKE_paint_curve_free((PaintCurve *)id);
			break;
	}

	/* avoid notifying on removed data */
	BKE_main_lock(bmain);

	if (free_notifier_reference_cb) {
		free_notifier_reference_cb(id);
	}

	if (remap_editor_id_reference_cb) {
		remap_editor_id_reference_cb(id, NULL);
	}

	BLI_remlink(lb, id);

	BKE_libblock_free_data(bmain, id);

	BKE_libraries_asset_subdata_remove(bmain, id);

	BKE_main_unlock(bmain);

	MEM_freeN(id);
}

void BKE_libblock_free(Main *bmain, void *idv)
{
	BKE_libblock_free_ex(bmain, idv, true);
}

void BKE_libblock_free_us(Main *bmain, void *idv)      /* test users */
{
	ID *id = idv;
	
	id_us_min(id);

	/* XXX This is a temp (2.77) hack so that we keep same behavior as in 2.76 regarding groups when deleting an object.
	 *     Since only 'user_one' usage of objects is groups, and only 'real user' usage of objects is scenes,
	 *     removing that 'user_one' tag when there is no more real (scene) users of an object ensures it gets
	 *     fully unlinked.
	 *     Otherwise, there is no real way to get rid of an object anymore - better handling of this is TODO.
	 */
	if ((GS(id->name) == ID_OB) && (id->us == 1)) {
		id_us_clear_real(id);
	}

	if (id->us == 0) {
		BKE_libblock_unlink(bmain, id, false);
		
		BKE_libblock_free(bmain, id);
	}
}

void BKE_libblock_delete(Main *bmain, void *idv)
{
	ListBase *lbarray[MAX_LIBARRAY];
	int base_count, i;

	base_count = set_listbasepointers(bmain, lbarray);
	BKE_main_id_tag_all(bmain, LIB_TAG_DOIT, false);

	/* First tag all datablocks directly from target lib.
     * Note that we go forward here, since we want to check dependencies before users (e.g. meshes before objetcs).
     * Avoids to have to loop twice. */
	for (i = 0; i < base_count; i++) {
		ListBase *lb = lbarray[i];
		ID *id;

		for (id = lb->first; id; id = id->next) {
			/* Note: in case we delete a library, we also delete all its datablocks! */
			if ((id == (ID *)idv) || (id->lib == (Library *)idv) || (id->tag & LIB_TAG_DOIT)) {
				id->tag |= LIB_TAG_DOIT;
				/* Will tag 'never NULL' users of this ID too.
				 * Note that we cannot use BKE_libblock_unlink() here, since it would ignore indirect (and proxy!)
				 * links, this can lead to nasty crashing here in second, actual deleting loop.
				 * Also, this will also flag users of deleted data that cannot be unlinked
				 * (object using deleted obdata, etc.), so that they also get deleted. */
				BKE_libblock_remap(bmain, id, NULL, ID_REMAP_FLAG_NEVER_NULL_USAGE | ID_REMAP_FORCE_NEVER_NULL_USAGE);
			}
		}
	}

	/* In usual reversed order, such that all usage of a given ID, even 'never NULL' ones, have been already cleared
	 * when we reach it (e.g. Objects being processed before meshes, they'll have already released their 'reference'
	 * over meshes when we come to freeing obdata). */
	for (i = base_count; i--; ) {
		ListBase *lb = lbarray[i];
		ID *id, *id_next;

		for (id = lb->first; id; id = id_next) {
			id_next = id->next;
			if (id->tag & LIB_TAG_DOIT) {
				if (id->us != 0) {
					printf("%s: deleting %s (%d)\n", __func__, id->name, id->us);
					BLI_assert(id->us == 0);
				}
				BKE_libblock_free(bmain, id);
			}
		}
	}
}

Main *BKE_main_new(void)
{
	Main *bmain = MEM_callocN(sizeof(Main), "new main");
	bmain->eval_ctx = DEG_evaluation_context_new(DAG_EVAL_VIEWPORT);
	bmain->lock = MEM_mallocN(sizeof(SpinLock), "main lock");
	BLI_spin_init((SpinLock *)bmain->lock);
	return bmain;
}

void BKE_main_free(Main *mainvar)
{
	/* also call when reading a file, erase all, etc */
	ListBase *lbarray[MAX_LIBARRAY];
	int a;

	MEM_SAFE_FREE(mainvar->blen_thumb);

	a = set_listbasepointers(mainvar, lbarray);
	while (a--) {
		ListBase *lb = lbarray[a];
		ID *id;
		
		while ( (id = lb->first) ) {
#if 1
			BKE_libblock_free_ex(mainvar, id, false);
#else
			/* errors freeing ID's can be hard to track down,
			 * enable this so valgrind will give the line number in its error log */
			switch (a) {
				case   0: BKE_libblock_free_ex(mainvar, id, false); break;
				case   1: BKE_libblock_free_ex(mainvar, id, false); break;
				case   2: BKE_libblock_free_ex(mainvar, id, false); break;
				case   3: BKE_libblock_free_ex(mainvar, id, false); break;
				case   4: BKE_libblock_free_ex(mainvar, id, false); break;
				case   5: BKE_libblock_free_ex(mainvar, id, false); break;
				case   6: BKE_libblock_free_ex(mainvar, id, false); break;
				case   7: BKE_libblock_free_ex(mainvar, id, false); break;
				case   8: BKE_libblock_free_ex(mainvar, id, false); break;
				case   9: BKE_libblock_free_ex(mainvar, id, false); break;
				case  10: BKE_libblock_free_ex(mainvar, id, false); break;
				case  11: BKE_libblock_free_ex(mainvar, id, false); break;
				case  12: BKE_libblock_free_ex(mainvar, id, false); break;
				case  13: BKE_libblock_free_ex(mainvar, id, false); break;
				case  14: BKE_libblock_free_ex(mainvar, id, false); break;
				case  15: BKE_libblock_free_ex(mainvar, id, false); break;
				case  16: BKE_libblock_free_ex(mainvar, id, false); break;
				case  17: BKE_libblock_free_ex(mainvar, id, false); break;
				case  18: BKE_libblock_free_ex(mainvar, id, false); break;
				case  19: BKE_libblock_free_ex(mainvar, id, false); break;
				case  20: BKE_libblock_free_ex(mainvar, id, false); break;
				case  21: BKE_libblock_free_ex(mainvar, id, false); break;
				case  22: BKE_libblock_free_ex(mainvar, id, false); break;
				case  23: BKE_libblock_free_ex(mainvar, id, false); break;
				case  24: BKE_libblock_free_ex(mainvar, id, false); break;
				case  25: BKE_libblock_free_ex(mainvar, id, false); break;
				case  26: BKE_libblock_free_ex(mainvar, id, false); break;
				case  27: BKE_libblock_free_ex(mainvar, id, false); break;
				case  28: BKE_libblock_free_ex(mainvar, id, false); break;
				case  29: BKE_libblock_free_ex(mainvar, id, false); break;
				case  30: BKE_libblock_free_ex(mainvar, id, false); break;
				case  31: BKE_libblock_free_ex(mainvar, id, false); break;
				case  32: BKE_libblock_free_ex(mainvar, id, false); break;
				case  33: BKE_libblock_free_ex(mainvar, id, false); break;
				default:
					BLI_assert(0);
					break;
			}
#endif
		}
	}

	BLI_spin_end((SpinLock *)mainvar->lock);
	MEM_freeN(mainvar->lock);
	DEG_evaluation_context_free(mainvar->eval_ctx);
	MEM_freeN(mainvar);
}

void BKE_main_lock(struct Main *bmain)
{
	BLI_spin_lock((SpinLock *) bmain->lock);
}

void BKE_main_unlock(struct Main *bmain)
{
	BLI_spin_unlock((SpinLock *) bmain->lock);
}

/**
 * Generates a raw .blend file thumbnail data from given image.
 *
 * \param bmain If not NULL, also store generated data in this Main.
 * \param img ImBuf image to generate thumbnail data from.
 * \return The generated .blend file raw thumbnail data.
 */
BlendThumbnail *BKE_main_thumbnail_from_imbuf(Main *bmain, ImBuf *img)
{
	BlendThumbnail *data = NULL;

	if (bmain) {
		MEM_SAFE_FREE(bmain->blen_thumb);
	}

	if (img) {
		const size_t sz = BLEN_THUMB_MEMSIZE(img->x, img->y);
		data = MEM_mallocN(sz, __func__);

		IMB_rect_from_float(img);  /* Just in case... */
		data->width = img->x;
		data->height = img->y;
		memcpy(data->rect, img->rect, sz - sizeof(*data));
	}

	if (bmain) {
		bmain->blen_thumb = data;
	}
	return data;
}

/**
 * Generates an image from raw .blend file thumbnail \a data.
 *
 * \param bmain Use this bmain->blen_thumb data if given \a data is NULL.
 * \param data Raw .blend file thumbnail data.
 * \return An ImBuf from given data, or NULL if invalid.
 */
ImBuf *BKE_main_thumbnail_to_imbuf(Main *bmain, BlendThumbnail *data)
{
	ImBuf *img = NULL;

	if (!data && bmain) {
		data = bmain->blen_thumb;
	}

	if (data) {
		/* Note: we cannot use IMB_allocFromBuffer(), since it tries to dupalloc passed buffer, which will fail
		 *       here (we do not want to pass the first two ints!). */
		img = IMB_allocImBuf((unsigned int)data->width, (unsigned int)data->height, 32, IB_rect | IB_metadata);
		memcpy(img->rect, data->rect, BLEN_THUMB_MEMSIZE(data->width, data->height) - sizeof(*data));
	}

	return img;
}

/**
 * Generates an empty (black) thumbnail for given Main.
 */
void BKE_main_thumbnail_create(struct Main *bmain)
{
	MEM_SAFE_FREE(bmain->blen_thumb);

	bmain->blen_thumb = MEM_callocN(BLEN_THUMB_MEMSIZE(BLEN_THUMB_SIZE, BLEN_THUMB_SIZE), __func__);
	bmain->blen_thumb->width = BLEN_THUMB_SIZE;
	bmain->blen_thumb->height = BLEN_THUMB_SIZE;
}

/* ***************** ID ************************ */
ID *BKE_libblock_find_name_ex(struct Main *bmain, const short type, const char *name)
{
	ListBase *lb = which_libbase(bmain, type);
	BLI_assert(lb != NULL);
	return BLI_findstring(lb, name, offsetof(ID, name) + 2);
}
ID *BKE_libblock_find_name(const short type, const char *name)
{
	return BKE_libblock_find_name_ex(G.main, type, name);
}


void id_sort_by_name(ListBase *lb, ID *id)
{
	ID *idtest;
	
	/* insert alphabetically */
	if (lb->first != lb->last) {
		BLI_remlink(lb, id);
		
		idtest = lb->first;
		while (idtest) {
			if (BLI_strcasecmp(idtest->name, id->name) > 0 || (idtest->lib && !id->lib)) {
				BLI_insertlinkbefore(lb, idtest, id);
				break;
			}
			idtest = idtest->next;
		}
		/* as last */
		if (idtest == NULL) {
			BLI_addtail(lb, id);
		}
	}
	
}

/**
 * Check to see if there is an ID with the same name as 'name'.
 * Returns the ID if so, if not, returns NULL
 */
static ID *is_dupid(ListBase *lb, ID *id, const char *name)
{
	ID *idtest = NULL;
	
	for (idtest = lb->first; idtest; idtest = idtest->next) {
		/* if idtest is not a lib */ 
		if (id != idtest && idtest->lib == NULL) {
			/* do not test alphabetic! */
			/* optimized */
			if (idtest->name[2] == name[0]) {
				if (STREQ(name, idtest->name + 2)) break;
			}
		}
	}
	
	return idtest;
}

/**
 * Check to see if an ID name is already used, and find a new one if so.
 * Return true if created a new name (returned in name).
 *
 * Normally the ID that's being check is already in the ListBase, so ID *id
 * points at the new entry.  The Python Library module needs to know what
 * the name of a datablock will be before it is appended; in this case ID *id
 * id is NULL
 */

static bool check_for_dupid(ListBase *lb, ID *id, char *name)
{
	ID *idtest;
	int nr = 0, a, left_len;
#define MAX_IN_USE 64
	bool in_use[MAX_IN_USE];
	/* to speed up finding unused numbers within [1 .. MAX_IN_USE - 1] */

	char left[MAX_ID_NAME + 8], leftest[MAX_ID_NAME + 8];

	while (true) {

		/* phase 1: id already exists? */
		idtest = is_dupid(lb, id, name);

		/* if there is no double, done */
		if (idtest == NULL) return false;

		/* we have a dup; need to make a new name */
		/* quick check so we can reuse one of first MAX_IN_USE - 1 ids if vacant */
		memset(in_use, false, sizeof(in_use));

		/* get name portion, number portion ("name.number") */
		left_len = BLI_split_name_num(left, &nr, name, '.');

		/* if new name will be too long, truncate it */
		if (nr > 999 && left_len > (MAX_ID_NAME - 8)) {  /* assumption: won't go beyond 9999 */
			left[MAX_ID_NAME - 8] = 0;
			left_len = MAX_ID_NAME - 8;
		}
		else if (left_len > (MAX_ID_NAME - 7)) {
			left[MAX_ID_NAME - 7] = 0;
			left_len = MAX_ID_NAME - 7;
		}

		for (idtest = lb->first; idtest; idtest = idtest->next) {
			int nrtest;
			if ( (id != idtest) &&
			     (idtest->lib == NULL) &&
			     (*name == *(idtest->name + 2)) &&
			     STREQLEN(name, idtest->name + 2, left_len) &&
			     (BLI_split_name_num(leftest, &nrtest, idtest->name + 2, '.') == left_len)
			     )
			{
				/* will get here at least once, otherwise is_dupid call above would have returned NULL */
				if (nrtest < MAX_IN_USE)
					in_use[nrtest] = true;  /* mark as used */
				if (nr <= nrtest)
					nr = nrtest + 1;    /* track largest unused */
			}
		}
		/* At this point, 'nr' will typically be at least 1. (but not always) */
		// BLI_assert(nr >= 1);

		/* decide which value of nr to use */
		for (a = 0; a < MAX_IN_USE; a++) {
			if (a >= nr) break;  /* stop when we've checked up to biggest */  /* redundant check */
			if (!in_use[a]) { /* found an unused value */
				nr = a;
				/* can only be zero if all potential duplicate names had
				 * nonzero numeric suffixes, which means name itself has
				 * nonzero numeric suffix (else no name conflict and wouldn't
				 * have got here), which means name[left_len] is not a null */
				break;
			}
		}
		/* At this point, nr is either the lowest unused number within [0 .. MAX_IN_USE - 1],
		 * or 1 greater than the largest used number if all those low ones are taken.
		 * We can't be bothered to look for the lowest unused number beyond (MAX_IN_USE - 1). */

		/* If the original name has no numeric suffix, 
		 * rather than just chopping and adding numbers, 
		 * shave off the end chars until we have a unique name.
		 * Check the null terminators match as well so we don't get Cube.000 -> Cube.00 */
		if (nr == 0 && name[left_len] == '\0') {
			int len;
			/* FIXME: this code will never be executed, because either nr will be
			 * at least 1, or name will not end at left_len! */
			BLI_assert(0);

			len = left_len - 1;
			idtest = is_dupid(lb, id, name);
			
			while (idtest && len > 1) {
				name[len--] = '\0';
				idtest = is_dupid(lb, id, name);
			}
			if (idtest == NULL) return true;
			/* otherwise just continue and use a number suffix */
		}
		
		if (nr > 999 && left_len > (MAX_ID_NAME - 8)) {
			/* this would overflow name buffer */
			left[MAX_ID_NAME - 8] = 0;
			/* left_len = MAX_ID_NAME - 8; */ /* for now this isn't used again */
			memcpy(name, left, sizeof(char) * (MAX_ID_NAME - 7));
			continue;
		}
		/* this format specifier is from hell... */
		BLI_snprintf(name, sizeof(id->name) - 2, "%s.%.3d", left, nr);

		return true;
	}

#undef MAX_IN_USE
}

/*
 * Only for local blocks: external en indirect blocks already have a
 * unique ID.
 *
 * return true: created a new name
 */

bool new_id(ListBase *lb, ID *id, const char *tname)
{
	bool result;
	char name[MAX_ID_NAME - 2];

	/* if library, don't rename */
	if (id->lib)
		return false;

	/* if no libdata given, look up based on ID */
	if (lb == NULL)
		lb = which_libbase(G.main, GS(id->name));

	/* if no name given, use name of current ID
	 * else make a copy (tname args can be const) */
	if (tname == NULL)
		tname = id->name + 2;

	BLI_strncpy(name, tname, sizeof(name));

	if (name[0] == '\0') {
		/* disallow empty names */
		BLI_strncpy(name, DATA_(ID_FALLBACK_NAME), sizeof(name));
	}
	else {
		/* disallow non utf8 chars,
		 * the interface checks for this but new ID's based on file names don't */
		BLI_utf8_invalid_strip(name, strlen(name));
	}

	result = check_for_dupid(lb, id, name);
	strcpy(id->name + 2, name);

	/* This was in 2.43 and previous releases
	 * however all data in blender should be sorted, not just duplicate names
	 * sorting should not hurt, but noting just incase it alters the way other
	 * functions work, so sort every time */
#if 0
	if (result)
		id_sort_by_name(lb, id);
#endif

	id_sort_by_name(lb, id);
	
	return result;
}

/**
 * Pull an ID out of a library (make it local). Only call this for IDs that
 * don't have other library users.
 */
void id_clear_lib_data_ex(Main *bmain, ID *id, bool id_in_mainlist)
{
	bNodeTree *ntree = NULL;

	BKE_id_lib_local_paths(bmain, id->lib, id);

	id_fake_user_clear(id);

	id->lib = NULL;
	MEM_SAFE_FREE(id->uuid);  /* Local ID have no more use for asset-related data. */
	id->tag &= ~(LIB_TAG_INDIRECT | LIB_TAG_EXTERN);
	if (id_in_mainlist)
		new_id(which_libbase(bmain, GS(id->name)), id, NULL);

	/* internal bNodeTree blocks inside ID types below
	 * also stores id->lib, make sure this stays in sync.
	 */
	ntree = ntreeFromID(id);

	if (ntree) {
		ntreeMakeLocal(ntree, false);
	}

	if (GS(id->name) == ID_OB) {
		Object *object = (Object *)id;
		if (object->proxy_from != NULL) {
			object->proxy_from->proxy = NULL;
			object->proxy_from->proxy_group = NULL;
		}
		object->proxy = object->proxy_from = object->proxy_group = NULL;
	}
}

void id_clear_lib_data(Main *bmain, ID *id)
{
	id_clear_lib_data_ex(bmain, id, true);
}

/* next to indirect usage in read/writefile also in editobject.c scene.c */
void BKE_main_id_clear_newpoins(Main *bmain)
{
	ListBase *lbarray[MAX_LIBARRAY];
	ID *id;
	int a;

	a = set_listbasepointers(bmain, lbarray);
	while (a--) {
		id = lbarray[a]->first;
		while (id) {
			id->newid = NULL;
			id->tag &= ~LIB_TAG_NEW;
			id = id->next;
		}
	}
}

static void lib_indirect_test_id(ID *id, const Library *lib)
{
#define LIBTAG(a) \
	if (a && a->id.lib) { a->id.tag &= ~LIB_TAG_INDIRECT; a->id.tag |= LIB_TAG_EXTERN; } (void)0
	
	if (id->lib) {
		/* datablocks that were indirectly related are now direct links
		 * without this, appending data that has a link to other data will fail to write */
		if (lib && id->lib->parent == lib) {
			id_lib_extern(id);
		}
		return;
	}
	
	if (GS(id->name) == ID_OB) {
		Object *ob = (Object *)id;
		Mesh *me;

		int a;

#if 0   /* XXX OLD ANIMSYS, NLASTRIPS ARE NO LONGER USED */
		/* XXX old animation system! -------------------------------------- */
		{
			bActionStrip *strip;
			for (strip = ob->nlastrips.first; strip; strip = strip->next) {
				LIBTAG(strip->object);
				LIBTAG(strip->act);
				LIBTAG(strip->ipo);
			}
		}
		/* XXX: new animation system needs something like this? */
#endif

		for (a = 0; a < ob->totcol; a++) {
			LIBTAG(ob->mat[a]);
		}
	
		LIBTAG(ob->dup_group);
		LIBTAG(ob->proxy);
		
		me = ob->data;
		LIBTAG(me);
	}

#undef LIBTAG
}

/** Make linked datablocks local.
 *
 * \param bmain Almost certainly G.main.
 * \param lib If not NULL, only make local datablocks from this library.
 * \param untagged_only If true, only make local datablocks not tagged with LIB_TAG_PRE_EXISTING.
 * \param set_fake If true, set fake user on all localized datablocks (except group and objects ones).
 */
void BKE_library_make_local(Main *bmain, const Library *lib, const bool untagged_only, const bool set_fake)
{
	ListBase *lbarray[MAX_LIBARRAY];
	ID *id, *idn;
	int a;

	a = set_listbasepointers(bmain, lbarray);
	while (a--) {
		id = lbarray[a]->first;
		
		while (id) {
			id->newid = NULL;
			idn = id->next;      /* id is possibly being inserted again */
			
			/* The check on the second line (LIB_TAG_PRE_EXISTING) is done so its
			 * possible to tag data you don't want to be made local, used for
			 * appending data, so any libdata already linked wont become local
			 * (very nasty to discover all your links are lost after appending)  
			 * */
			if (id->tag & (LIB_TAG_EXTERN | LIB_TAG_INDIRECT | LIB_TAG_NEW) &&
			    ((untagged_only == false) || !(id->tag & LIB_TAG_PRE_EXISTING)))
			{
				if (lib == NULL || id->lib == lib) {
					if (id->lib) {
						/* for Make Local > All we should be calling id_make_local,
						 * but doing that breaks append (see #36003 and #36006), we
						 * we should make it work with all datablocks and id.us==0 */
						id_clear_lib_data(bmain, id); /* sets 'id->tag' */

						/* why sort alphabetically here but not in
						 * id_clear_lib_data() ? - campbell */
						id_sort_by_name(lbarray[a], id);
					}
					else {
						id->tag &= ~(LIB_TAG_EXTERN | LIB_TAG_INDIRECT | LIB_TAG_NEW);
					}
				}

				if (set_fake) {
					if (!ELEM(GS(id->name), ID_OB, ID_GR)) {
						/* do not set fake user on objects, groups (instancing) */
						id_fake_user_set(id);
					}
				}
			}

			id = idn;
		}
	}

	a = set_listbasepointers(bmain, lbarray);
	while (a--) {
		for (id = lbarray[a]->first; id; id = id->next)
			lib_indirect_test_id(id, lib);
	}
}

/* Asset managing - TODO: we most likely want to turn this into a hashing at some point, could become a bit slow
 *                        when having huge assets (or many of them)... */
void BKE_library_asset_repository_init(Library *lib, const AssetEngineType *aet, const char *repo_root)
{
	BKE_library_asset_repository_free(lib);
	lib->asset_repository = MEM_mallocN(sizeof(*lib->asset_repository), __func__);

	BLI_strncpy(lib->asset_repository->asset_engine, aet->idname, sizeof(lib->asset_repository->asset_engine));
	lib->asset_repository->asset_engine_version = aet->version;
	BLI_strncpy(lib->asset_repository->root, repo_root, sizeof(lib->asset_repository->root));

	BLI_listbase_clear(&lib->asset_repository->assets);
}

void BKE_library_asset_repository_clear(Library *lib)
{
	if (lib->asset_repository) {
		for (AssetRef *aref; (aref = BLI_pophead(&lib->asset_repository->assets)); ) {
			BLI_freelistN(&aref->id_list);
			MEM_freeN(aref);
		}
	}
}

void BKE_library_asset_repository_free(Library *lib)
{
	if (lib->asset_repository) {
		BKE_library_asset_repository_clear(lib);
		MEM_freeN(lib->asset_repository);
		lib->asset_repository = NULL;
	}
}

AssetRef *BKE_library_asset_repository_asset_add(Library *lib, const void *idv)
{
	const ID *id = idv;
	BLI_assert(id->uuid != NULL);

	AssetRef *aref = BKE_library_asset_repository_asset_find(lib, idv);
	if (!aref) {
		aref = MEM_callocN(sizeof(*aref), __func__);
		aref->uuid = *id->uuid;
		BKE_library_asset_repository_subdata_add(aref, idv);
		BLI_addtail(&lib->asset_repository->assets, aref);
	}

	return aref;
}

AssetRef *BKE_library_asset_repository_asset_find(Library *lib, const void *idv)
{
	const ID *id = idv;
	BLI_assert(id->uuid != NULL);

	for (AssetRef *aref = lib->asset_repository->assets.first; aref; aref = aref->next) {
		if (ASSETUUID_COMPARE(&aref->uuid, id->uuid)) {
#ifndef NDEBUG
			LinkData *link = aref->id_list.first;
			BLI_assert(link && (link->data == idv));
#endif
			return aref;
		}
	}
	return NULL;
}

void BKE_library_asset_repository_asset_remove(Library *lib, const void *idv)
{
	AssetRef *aref = BKE_library_asset_repository_asset_find(lib, idv);
	BLI_remlink(&lib->asset_repository->assets, aref);
	BLI_freelistN(&aref->id_list);
	MEM_freeN(aref);
}

void BKE_library_asset_repository_subdata_add(AssetRef *aref, const void *idv)
{
	if (BLI_findptr(&aref->id_list, idv, offsetof(LinkData, data)) == NULL) {
		BLI_addtail(&aref->id_list, BLI_genericNodeN((void *)idv));
	}
}

void BKE_library_asset_repository_subdata_remove(AssetRef *aref, const void *idv)
{
	LinkData *link = BLI_findptr(&aref->id_list, idv, offsetof(LinkData, data));
	if (link) {
		BLI_freelinkN(&aref->id_list, link);
	}
}

void BKE_libraries_asset_subdata_remove(Main *bmain, const void *idv)
{
	const ID *id = idv;

	if (id->lib == NULL) {
		return;
	}

	ListBase *lb = which_libbase(bmain, ID_LI);
	for (Library *lib = lb->first; lib; lib = lib->id.next) {
		if (lib->asset_repository) {
			for (AssetRef *aref = lib->asset_repository->assets.first; aref; aref = aref->next) {
				BLI_freelinkN(&aref->id_list, BLI_findptr(&aref->id_list, idv, offsetof(LinkData, data)));
			}
		}
	}
}

void BKE_libraries_asset_repositories_clear(Main *bmain)
{
	ListBase *lb = which_libbase(bmain, ID_LI);
	for (Library *lib = lb->first; lib; lib = lib->id.next) {
		BKE_library_asset_repository_clear(lib);
	}
	BKE_main_id_tag_all(bmain, LIB_TAG_ASSET, false);
}

static int library_asset_dependencies_rebuild_cb(void *userdata, ID *id_self, ID **idp, int UNUSED(cd_flag))
{
	if (!idp || !*idp) {
		return IDWALK_RET_NOP;
	}

	AssetRef *aref = userdata;
	ID *id = *idp;

	if (id->uuid) {
		return IDWALK_RET_STOP_RECURSION;
	}

	printf("%s (from %s)\n", id->name, id_self->name);

	BKE_library_asset_repository_subdata_add(aref, (const void *)id);
	id->tag |= LIB_TAG_ASSET;
	return IDWALK_RET_NOP;
}

static void library_asset_dependencies_rebuild(ID *asset)
{
	Library *lib = asset->lib;
	BLI_assert(lib->asset_repository);

	asset->tag |= LIB_TAG_ASSET;

	AssetRef *aref = BKE_library_asset_repository_asset_add(lib, asset);

	BKE_library_foreach_ID_link(asset, library_asset_dependencies_rebuild_cb, aref, IDWALK_RECURSE);
}

void BKE_libraries_asset_repositories_rebuild(Main *bmain)
{
	ListBase *lbarray[MAX_LIBARRAY];
	ID *id;
	int a;

	BKE_libraries_asset_repositories_clear(bmain);

	a = set_listbasepointers(bmain, lbarray);
	while (a--) {
		for (id = lbarray[a]->first; id; id = id->next) {
			if (id->uuid) {
				library_asset_dependencies_rebuild(id);
			}
		}
	}
}

AssetRef *BKE_libraries_asset_repository_uuid_find(Main *bmain, const AssetUUID *uuid)
{
	ListBase *lb = which_libbase(bmain, ID_LI);
	for (Library *lib = lb->first; lib; lib = lib->id.next) {
		for (AssetRef *aref = lib->asset_repository->assets.first; aref; aref = aref->next) {
			if (ASSETUUID_COMPARE(&aref->uuid, uuid)) {
#ifndef NDEBUG
				LinkData *link = aref->id_list.first;
				BLI_assert(link && ((ID *)link->data)->uuid && ASSETUUID_COMPARE(((ID *)link->data)->uuid, uuid));
#endif
				return aref;
			}
		}
	}
	return NULL;
}

/**
 * Use after setting the ID's name
 * When name exists: call 'new_id'
 */
void BLI_libblock_ensure_unique_name(Main *bmain, const char *name)
{
	ListBase *lb;
	ID *idtest;


	lb = which_libbase(bmain, GS(name));
	if (lb == NULL) return;
	
	/* search for id */
	idtest = BLI_findstring(lb, name + 2, offsetof(ID, name) + 2);

	if (idtest && !new_id(lb, idtest, idtest->name + 2)) {
		id_sort_by_name(lb, idtest);
	}
}

/**
 * Sets the name of a block to name, suitably adjusted for uniqueness.
 */
void BKE_libblock_rename(Main *bmain, ID *id, const char *name)
{
	ListBase *lb = which_libbase(bmain, GS(id->name));
	new_id(lb, id, name);
}

/**
 * Returns in name the name of the block, with a 3-character prefix prepended
 * indicating whether it comes from a library, has a fake user, or no users.
 */
void BKE_id_ui_prefix(char name[MAX_ID_NAME + 1], const ID *id)
{
	name[0] = id->lib ? (ID_MISSING(id) ? 'M' : 'L') : ' ';
	name[1] = (id->flag & LIB_FAKEUSER) ? 'F' : ((id->us == 0) ? '0' : ' ');
	name[2] = ' ';

	strcpy(name + 3, id->name + 2);
}

void BKE_library_filepath_set(Library *lib, const char *filepath)
{
	/* in some cases this is used to update the absolute path from the
	 * relative */
	if (lib->name != filepath) {
		BLI_strncpy(lib->name, filepath, sizeof(lib->name));
	}

	BLI_strncpy(lib->filepath, filepath, sizeof(lib->filepath));

	/* not essential but set filepath is an absolute copy of value which
	 * is more useful if its kept in sync */
	if (BLI_path_is_rel(lib->filepath)) {
		/* note that the file may be unsaved, in this case, setting the
		 * filepath on an indirectly linked path is not allowed from the
		 * outliner, and its not really supported but allow from here for now
		 * since making local could cause this to be directly linked - campbell
		 */
		const char *basepath = lib->parent ? lib->parent->filepath : G.main->name;
		BLI_path_abs(lib->filepath, basepath);
	}
}
