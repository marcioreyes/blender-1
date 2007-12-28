/* particle_system.c
 *
 *
 * $Id: particle_system.c $
 *
 * ***** BEGIN GPL/BL DUAL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version. The Blender
 * Foundation also sells licenses for use in proprietary software under
 * the Blender License.  See http://www.blender.org/BL/ for information
 * about this.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * The Original Code is Copyright (C) 2007 by Janne Karhu.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL/BL DUAL LICENSE BLOCK *****
 */

#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "DNA_particle_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_force.h"
#include "DNA_object_types.h"
#include "DNA_material_types.h"
#include "DNA_ipo_types.h"
#include "DNA_curve_types.h"
#include "DNA_group_types.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

#include "BLI_rand.h"
#include "BLI_jitter.h"
#include "BLI_arithb.h"
#include "BLI_blenlib.h"
#include "BLI_kdtree.h"
#include "BLI_linklist.h"
#include "BLI_threads.h"

#include "BKE_anim.h"
#include "BKE_bad_level_calls.h"
#include "BKE_cdderivedmesh.h"
#include "BKE_displist.h"

#include "BKE_particle.h"
#include "BKE_global.h"
#include "BKE_utildefines.h"
#include "BKE_DerivedMesh.h"
#include "BKE_object.h"
#include "BKE_material.h"
#include "BKE_ipo.h"
#include "BKE_softbody.h"
#include "BKE_depsgraph.h"
#include "BKE_lattice.h"
#include "BKE_pointcache.h"
#include "BKE_mesh.h"
#include "BKE_modifier.h"

#include "BSE_headerbuttons.h"

#include "blendef.h"

#include "RE_shader_ext.h"

/************************************************/
/*			Reacting to system events			*/
/************************************************/

static int get_current_display_percentage(ParticleSystem *psys)
{
	ParticleSettings *part=psys->part;

	if(psys->renderdata || (part->child_nbr && part->childtype)) 
		return 100;

	if(part->phystype==PART_PHYS_KEYED){
		if(psys->flag & PSYS_FIRST_KEYED)
			return psys->part->disp;
		else
			return 100;
	}
	else
		return psys->part->disp;
}

static void alloc_particles(Object *ob, ParticleSystem *psys, int new_totpart)
{
	ParticleData *newpars = 0, *pa;
	int i, totpart, totsaved = 0;

	if(new_totpart<0) {
		if(psys->part->distr==PART_DISTR_GRID) {
			totpart= psys->part->grid_res;
			totpart*=totpart*totpart;
		}
		else
			totpart=psys->part->totpart;
	}
	else
		totpart=new_totpart;

	if(totpart)
		newpars= MEM_callocN(totpart*sizeof(ParticleData), "particles");
	if(psys->particles) {
		totsaved=MIN2(psys->totpart,totpart);
		/*save old pars*/
		if(totsaved)
			memcpy(newpars,psys->particles,totsaved*sizeof(ParticleData));

		for(i=totsaved, pa=psys->particles+totsaved; i<psys->totpart; i++, pa++)
			if(pa->hair) MEM_freeN(pa->hair);

		MEM_freeN(psys->particles);
	}
	psys->particles=newpars;

	if(psys->child) {
		MEM_freeN(psys->child);
		psys->child=0;
		psys->totchild=0;
	}

	psys->totpart=totpart;
}

static int get_alloc_child_particles_tot(ParticleSystem *psys)
{
	int child_nbr;

	if(!psys->part->childtype)
		return 0;

	child_nbr= (psys->renderdata)? psys->part->ren_child_nbr: psys->part->child_nbr;
	return psys->totpart*child_nbr;
}

static void alloc_child_particles(ParticleSystem *psys, int tot)
{
	if(psys->child){
		MEM_freeN(psys->child);
		psys->child=0;
		psys->totchild=0;
	}

	if(psys->part->childtype) {
		psys->totchild= tot;
		if(psys->totchild)
			psys->child= MEM_callocN(psys->totchild*sizeof(ChildParticle), "child_particles");
	}
}

/* only run this if from == PART_FROM_FACE */
void psys_calc_dmfaces(Object *ob, DerivedMesh *dm, ParticleSystem *psys)
{
	/* use for building derived mesh face-origin info,
	node - the allocated links - total derived mesh face count 
	node_array - is the array of nodes alligned with the base mesh's faces, so each original face can reference its derived faces
	*/
	Mesh *me= (Mesh*)ob->data;
	ParticleData *pa= 0;
	int p;
	
	/* CACHE LOCATIONS */
	if(!dm->deformedOnly) {
		/* Will use later to speed up subsurf/derivedmesh */
	
		int tot_dm_face = dm->getNumFaces(dm);
		int totface = me->totface;
		int *origindex = DM_get_face_data_layer(dm, CD_ORIGINDEX);
		int i;
		LinkNode *node, *node_dm_faces, **node_array;
		
		node_dm_faces = node = MEM_callocN(sizeof(LinkNode)*tot_dm_face, "faceindicies");
		node_array = MEM_callocN(sizeof(LinkNode *)*totface, "faceindicies array");
		
		for(i=0; i < tot_dm_face; i++, origindex++, node++) {
			node->link = (void *)i; // or use the index?
			if(*origindex != -1) {
				if(node_array[*origindex]) {
					/* prepend */
					node->next = node_array[*origindex];
					node_array[*origindex] = node;
				} else {
					node_array[*origindex] = node;
				}
			}
		}
		
		/* cache the faces! */


		for(p=0,pa=psys->particles; p<psys->totpart; p++,pa++) {
			//i = pa->num;
			//if (i<totface) // should never happen
			i = psys_particle_dm_face_lookup(ob, dm, pa->num, pa->fuv, node_array[pa->num]);
			pa->num_dmcache = i;
		}

		//for (i=0; i < totface; i++) {
		//	i = psys_particle_dm_face_lookup(ob, dm, node_array[], fuv, (LinkNode*)NULL);
		//}
		MEM_freeN(node_array);
		MEM_freeN(node_dm_faces);
		
	} else {
		/* set the num_dmcache to an invalid value, just incase */
		/* TODO PARTICLE, make the following line unnecessary, each function should know to use the num or num_dmcache */
		
		/*
		for(p=0,pa=psys->particles; p<psys->totpart; p++,pa++) {
			pa->num_dmcache = pa->num;
		}
		*/
		for(p=0,pa=psys->particles; p<psys->totpart; p++,pa++) {
			pa->num_dmcache = -1;
		}
	}
}

static void distribute_particles_in_grid(DerivedMesh *dm, ParticleSystem *psys)
{
	ParticleData *pa=0;
	float min[3], max[3], delta[3], d;
	MVert *mv, *mvert = dm->getVertDataArray(dm,0);
	int totvert=dm->getNumVerts(dm), from=psys->part->from;
	int i, j, k, p, res=psys->part->grid_res, size[3], axis;

	mv=mvert;

	/* find bounding box of dm */
	VECCOPY(min,mv->co);
	VECCOPY(max,mv->co);
	mv++;

	for(i=1; i<totvert; i++, mv++){
		min[0]=MIN2(min[0],mv->co[0]);
		min[1]=MIN2(min[1],mv->co[1]);
		min[2]=MIN2(min[2],mv->co[2]);

		max[0]=MAX2(max[0],mv->co[0]);
		max[1]=MAX2(max[1],mv->co[1]);
		max[2]=MAX2(max[2],mv->co[2]);
	}

	VECSUB(delta,max,min);

	/* determine major axis */
	axis = (delta[0]>=delta[1])?0:((delta[1]>=delta[2])?1:2);

	d = delta[axis]/(float)res;

	size[axis]=res;
	size[(axis+1)%3]=(int)ceil(delta[(axis+1)%3]/d);
	size[(axis+2)%3]=(int)ceil(delta[(axis+2)%3]/d);

	/* float errors grrr.. */
	size[(axis+1)%3] = MIN2(size[(axis+1)%3],res);
	size[(axis+2)%3] = MIN2(size[(axis+2)%3],res);

	min[0]+=d/2.0f;
	min[1]+=d/2.0f;
	min[2]+=d/2.0f;

	for(i=0,p=0,pa=psys->particles; i<res; i++){
		for(j=0; j<res; j++){
			for(k=0; k<res; k++,p++,pa++){
				pa->fuv[0]=min[0]+(float)i*d;
				pa->fuv[1]=min[1]+(float)j*d;
				pa->fuv[2]=min[2]+(float)k*d;
				pa->flag |= PARS_UNEXIST;
				pa->loop=0; /* abused in volume calculation */
			}
		}
	}

	/* enable particles near verts/edges/faces/inside surface */
	if(from==PART_FROM_VERT){
		float vec[3];

		pa=psys->particles;

		min[0]-=d/2.0f;
		min[1]-=d/2.0f;
		min[2]-=d/2.0f;

		for(i=0,mv=mvert; i<totvert; i++,mv++){
			VecSubf(vec,mv->co,min);
			vec[0]/=delta[0];
			vec[1]/=delta[1];
			vec[2]/=delta[2];
			(pa	+((int)(vec[0]*(size[0]-1))*res
				+(int)(vec[1]*(size[1]-1)))*res
				+(int)(vec[2]*(size[2]-1)))->flag &= ~PARS_UNEXIST;
		}
	}
	else if(ELEM(from,PART_FROM_FACE,PART_FROM_VOLUME)){
		float co1[3], co2[3];

		MFace *mface=0;
		float v1[3], v2[3], v3[3], v4[4], lambda;
		int a, a1, a2, a0mul, a1mul, a2mul, totface;
		int amax= from==PART_FROM_FACE ? 3 : 1;

		totface=dm->getNumFaces(dm);
		mface=dm->getFaceDataArray(dm,CD_MFACE);
		
		for(a=0; a<amax; a++){
			if(a==0){ a0mul=res*res; a1mul=res; a2mul=1; }
			else if(a==1){ a0mul=res; a1mul=1; a2mul=res*res; }
			else{ a0mul=1; a1mul=res*res; a2mul=res; }

			for(a1=0; a1<size[(a+1)%3]; a1++){
				for(a2=0; a2<size[(a+2)%3]; a2++){
					mface=dm->getFaceDataArray(dm,CD_MFACE);

					pa=psys->particles + a1*a1mul + a2*a2mul;
					VECCOPY(co1,pa->fuv);
					co1[a]-=d/2.0f;
					VECCOPY(co2,co1);
					co2[a]+=delta[a] + 0.001f*d;
					co1[a]-=0.001f*d;
					
					/* lets intersect the faces */
					for(i=0; i<totface; i++,mface++){
						VECCOPY(v1,mvert[mface->v1].co);
						VECCOPY(v2,mvert[mface->v2].co);
						VECCOPY(v3,mvert[mface->v3].co);

						if(AxialLineIntersectsTriangle(a,co1, co2, v2, v3, v1, &lambda)){
							if(from==PART_FROM_FACE)
								(pa+(int)(lambda*size[a])*a0mul)->flag &= ~PARS_UNEXIST;
							else /* store number of intersections */
								(pa+(int)(lambda*size[a])*a0mul)->loop++;
						}
						
						if(mface->v4){
							VECCOPY(v4,mvert[mface->v4].co);

							if(AxialLineIntersectsTriangle(a,co1, co2, v4, v1, v3, &lambda)){
								if(from==PART_FROM_FACE)
									(pa+(int)(lambda*size[a])*a0mul)->flag &= ~PARS_UNEXIST;
								else
									(pa+(int)(lambda*size[a])*a0mul)->loop++;
							}
						}
					}

					if(from==PART_FROM_VOLUME){
						int in=pa->loop%2;
						if(in) pa->loop++;
						for(i=0; i<size[0]; i++){
							if(in || (pa+i*a0mul)->loop%2)
								(pa+i*a0mul)->flag &= ~PARS_UNEXIST;
							/* odd intersections == in->out / out->in */
							/* even intersections -> in stays same */
							in=(in + (pa+i*a0mul)->loop) % 2;
						}
					}
				}
			}
		}
	}

	if(psys->part->flag & PART_GRID_INVERT){
		for(i=0,pa=psys->particles; i<size[0]; i++){
			for(j=0; j<size[1]; j++){
				pa=psys->particles + res*(i*res + j);
				for(k=0; k<size[2]; k++, pa++){
					pa->flag ^= PARS_UNEXIST;
				}
			}
		}
	}
}

/* modified copy from effect.c */
static void init_mv_jit(float *jit, int num, int seed2, float amount)
{
	RNG *rng;
	float *jit2, x, rad1, rad2, rad3;
	int i, num2;

	if(num==0) return;

	rad1= (float)(1.0/sqrt((float)num));
	rad2= (float)(1.0/((float)num));
	rad3= (float)sqrt((float)num)/((float)num);

	rng = rng_new(31415926 + num + seed2);
	x= 0;
        num2 = 2 * num;
	for(i=0; i<num2; i+=2) {
	
		jit[i]= x + amount*rad1*(0.5f - rng_getFloat(rng));
		jit[i+1]= i/(2.0f*num) + amount*rad1*(0.5f - rng_getFloat(rng));
		
		jit[i]-= (float)floor(jit[i]);
		jit[i+1]-= (float)floor(jit[i+1]);
		
		x+= rad3;
		x -= (float)floor(x);
	}

	jit2= MEM_mallocN(12 + 2*sizeof(float)*num, "initjit");

	for (i=0 ; i<4 ; i++) {
		BLI_jitterate1(jit, jit2, num, rad1);
		BLI_jitterate1(jit, jit2, num, rad1);
		BLI_jitterate2(jit, jit2, num, rad2);
	}
	MEM_freeN(jit2);
	rng_free(rng);
}

static void psys_uv_to_w(float u, float v, int quad, float *w)
{
	float vert[4][3], co[3];

	if(!quad) {
		if(u+v > 1.0f)
			v= 1.0f-v;
		else
			u= 1.0f-u;
	}

	vert[0][0]= 0.0f; vert[0][1]= 0.0f; vert[0][2]= 0.0f;
	vert[1][0]= 1.0f; vert[1][1]= 0.0f; vert[1][2]= 0.0f;
	vert[2][0]= 1.0f; vert[2][1]= 1.0f; vert[2][2]= 0.0f;

	co[0]= u;
	co[1]= v;
	co[2]= 0.0f;

	if(quad) {
		vert[3][0]= 0.0f; vert[3][1]= 1.0f; vert[3][2]= 0.0f;
		MeanValueWeights(vert, 4, co, w);
	}
	else {
		MeanValueWeights(vert, 3, co, w);
		w[3]= 0.0f;
	}
}

static int binary_search_distribution(float *sum, int n, float value)
{
	int mid, low=0, high=n;

	while(low <= high) {
		mid= (low + high)/2;
		if(sum[mid] <= value && value <= sum[mid+1])
			return mid;
		else if(sum[mid] > value)
			high= mid - 1;
		else if(sum[mid] < value)
			low= mid + 1;
		else
			return mid;
	}

	return low;
}

/* note: this function must be thread safe, for from == PART_FROM_CHILD */
#define ONLY_WORKING_WITH_PA_VERTS 0
void psys_thread_distribute_particle(ParticleThread *thread, ParticleData *pa, ChildParticle *cpa, int p)
{
	ParticleThreadContext *ctx= thread->ctx;
	Object *ob= ctx->ob;
	DerivedMesh *dm= ctx->dm;
	ParticleData *tpa;
	ParticleSettings *part= ctx->psys->part;
	float *v1, *v2, *v3, *v4, nor[3], orco1[3], co1[3], co2[3], nor1[3], ornor1[3];
	float cur_d, min_d;
	int from= ctx->from;
	int cfrom= ctx->cfrom;
	int distr= ctx->distr;
	int i, intersect, tot;

	if(from == PART_FROM_VERT) {
		/* TODO_PARTICLE - use original index */
		pa->num= ctx->index[p];
		pa->fuv[0] = 1.0f;
		pa->fuv[1] = pa->fuv[2] = pa->fuv[3] = 0.0;
		//pa->verts[0] = pa->verts[1] = pa->verts[2] = 0;

#if ONLY_WORKING_WITH_PA_VERTS
		if(ctx->tree){
			KDTreeNearest ptn[3];
			int w, maxw;

			psys_particle_on_dm(ctx->ob,ctx->dm,from,pa->num,pa->num_dmcache,pa->fuv,pa->foffset,co1,0,0,0,orco1,0);
			transform_mesh_orco_verts((Mesh*)ob->data, &orco1, 1, 1);
			maxw = BLI_kdtree_find_n_nearest(ctx->tree,3,orco1,NULL,ptn);

			for(w=0; w<maxw; w++){
				pa->verts[w]=ptn->num;
			}
		}
#endif
	}
	else if(from == PART_FROM_FACE || from == PART_FROM_VOLUME) {
		MFace *mface;

		pa->num = i = ctx->index[p];
		mface = dm->getFaceData(dm,i,CD_MFACE);
		
		switch(distr){
		case PART_DISTR_JIT:
			ctx->jitoff[i] = fmod(ctx->jitoff[i],(float)ctx->jitlevel);
			psys_uv_to_w(ctx->jit[2*(int)ctx->jitoff[i]], ctx->jit[2*(int)ctx->jitoff[i]+1], mface->v4, pa->fuv);
			ctx->jitoff[i]++;
			//ctx->jitoff[i]=(float)fmod(ctx->jitoff[i]+ctx->maxweight/ctx->weight[i],(float)ctx->jitlevel);
			break;
		case PART_DISTR_RAND:
			psys_uv_to_w(rng_getFloat(thread->rng), rng_getFloat(thread->rng), mface->v4, pa->fuv);
			break;
		}
		pa->foffset= 0.0f;
		
		/*
		pa->verts[0] = mface->v1;
		pa->verts[1] = mface->v2;
		pa->verts[2] = mface->v3;
		*/
		
		/* experimental */
		if(from==PART_FROM_VOLUME){
			MVert *mvert=dm->getVertDataArray(dm,CD_MVERT);

			tot=dm->getNumFaces(dm);

			psys_interpolate_face(mvert,mface,0,0,pa->fuv,co1,nor,0,0,0,0);

			Normalize(nor);
			VecMulf(nor,-100.0);

			VECADD(co2,co1,nor);

			min_d=2.0;
			intersect=0;

			for(i=0,mface=dm->getFaceDataArray(dm,CD_MFACE); i<tot; i++,mface++){
				if(i==pa->num) continue;

				v1=mvert[mface->v1].co;
				v2=mvert[mface->v2].co;
				v3=mvert[mface->v3].co;

				if(LineIntersectsTriangle(co1, co2, v2, v3, v1, &cur_d, 0)){
					if(cur_d<min_d){
						min_d=cur_d;
						pa->foffset=cur_d*50.0f; /* to the middle of volume */
						intersect=1;
					}
				}
				if(mface->v4){
					v4=mvert[mface->v4].co;

					if(LineIntersectsTriangle(co1, co2, v4, v1, v3, &cur_d, 0)){
						if(cur_d<min_d){
							min_d=cur_d;
							pa->foffset=cur_d*50.0f; /* to the middle of volume */
							intersect=1;
						}
					}
				}
			}
			if(intersect==0)
				pa->foffset=0.0;
			else switch(distr){
				case PART_DISTR_JIT:
					pa->foffset*= ctx->jit[2*(int)ctx->jitoff[i]];
					break;
				case PART_DISTR_RAND:
					pa->foffset*=BLI_frand();
					break;
			}
		}
	}
	else if(from == PART_FROM_PARTICLE) {
		//pa->verts[0]=0; /* not applicable */
		//pa->verts[1]=0;
		//pa->verts[2]=0;

		tpa=ctx->tpars+ctx->index[p];
		pa->num=ctx->index[p];
		pa->fuv[0]=tpa->fuv[0];
		pa->fuv[1]=tpa->fuv[1];
		/* abusing foffset a little for timing in near reaction */
		pa->foffset=ctx->weight[ctx->index[p]];
		ctx->weight[ctx->index[p]]+=ctx->maxweight;
	}
	else if(from == PART_FROM_CHILD) {
		MFace *mf;

		if(ctx->index[p] < 0) {
			cpa->num=0;
			cpa->fuv[0]=cpa->fuv[1]=cpa->fuv[2]=cpa->fuv[3]=0.0f;
			cpa->pa[0]=cpa->pa[1]=cpa->pa[2]=cpa->pa[3]=0;
			cpa->rand[0]=cpa->rand[1]=cpa->rand[2]=0.0f;
			return;
		}

		mf= dm->getFaceData(dm, ctx->index[p], CD_MFACE);

		//switch(distr){
		//	case PART_DISTR_JIT:
		//		i=index[p];
		//		psys_uv_to_w(ctx->jit[2*(int)ctx->jitoff[i]], ctx->jit[2*(int)ctx->jitoff[i]+1], mf->v4, cpa->fuv);
		//		ctx->jitoff[i]=(float)fmod(ctx->jitoff[i]+ctx->maxweight/ctx->weight[i],(float)ctx->jitlevel);
		//		break;
		//	case PART_DISTR_RAND:
				psys_uv_to_w(rng_getFloat(thread->rng), rng_getFloat(thread->rng), mf->v4, cpa->fuv);
		//		break;
		//}

		cpa->rand[0] = rng_getFloat(thread->rng);
		cpa->rand[1] = rng_getFloat(thread->rng);
		cpa->rand[2] = rng_getFloat(thread->rng);
		cpa->num = ctx->index[p];

		if(ctx->tree){
			KDTreeNearest ptn[10];
			int w,maxw, do_seams;
			float maxd,mind,dd,totw=0.0;
			int parent[10];
			float pweight[10];

			do_seams= (part->flag&PART_CHILD_SEAMS && ctx->seams);

			psys_particle_on_dm(ob,dm,cfrom,cpa->num,DMCACHE_ISCHILD,cpa->fuv,cpa->foffset,co1,nor1,0,0,orco1,ornor1);
			transform_mesh_orco_verts((Mesh*)ob->data, &orco1, 1, 1);
			maxw = BLI_kdtree_find_n_nearest(ctx->tree,(do_seams)?10:4,orco1,ornor1,ptn);

			maxd=ptn[maxw-1].dist;
			mind=ptn[0].dist;
			dd=maxd-mind;
			
			/* the weights here could be done better */
			for(w=0; w<maxw; w++){
				parent[w]=ptn[w].index;
				pweight[w]=(float)pow(2.0,(double)(-6.0f*ptn[w].dist/maxd));
				//pweight[w]= (1.0f - ptn[w].dist*ptn[w].dist/(maxd*maxd));
				//pweight[w] *= pweight[w];
			}
			for(;w<10; w++){
				parent[w]=-1;
				pweight[w]=0.0f;
			}
			if(do_seams){
				ParticleSeam *seam=ctx->seams;
				float temp[3],temp2[3],tan[3];
				float inp,cur_len,min_len=10000.0f;
				int min_seam=0, near_vert=0;
				/* find closest seam */
				for(i=0; i<ctx->totseam; i++, seam++){
					VecSubf(temp,co1,seam->v0);
					inp=Inpf(temp,seam->dir)/seam->length2;
					if(inp<0.0f){
						cur_len=VecLenf(co1,seam->v0);
					}
					else if(inp>1.0f){
						cur_len=VecLenf(co1,seam->v1);
					}
					else{
						VecCopyf(temp2,seam->dir);
						VecMulf(temp2,inp);
						cur_len=VecLenf(temp,temp2);
					}
					if(cur_len<min_len){
						min_len=cur_len;
						min_seam=i;
						if(inp<0.0f) near_vert=-1;
						else if(inp>1.0f) near_vert=1;
						else near_vert=0;
					}
				}
				seam=ctx->seams+min_seam;
				
				VecCopyf(temp,seam->v0);
				
				if(near_vert){
					if(near_vert==-1)
						VecSubf(tan,co1,seam->v0);
					else{
						VecSubf(tan,co1,seam->v1);
						VecCopyf(temp,seam->v1);
					}

					Normalize(tan);
				}
				else{
					VecCopyf(tan,seam->tan);
					VecSubf(temp2,co1,temp);
					if(Inpf(tan,temp2)<0.0f)
						VecMulf(tan,-1.0f);
				}
				for(w=0; w<maxw; w++){
					VecSubf(temp2,ptn[w].co,temp);
					if(Inpf(tan,temp2)<0.0f){
						parent[w]=-1;
						pweight[w]=0.0f;
					}
				}

			}

			for(w=0,i=0; w<maxw && i<4; w++){
				if(parent[w]>=0){
					cpa->pa[i]=parent[w];
					cpa->w[i]=pweight[w];
					totw+=pweight[w];
					i++;
				}
			}
			for(;i<4; i++){
				cpa->pa[i]=-1;
				cpa->w[i]=0.0f;
			}

			if(totw>0.0f) for(w=0; w<4; w++)
				cpa->w[w]/=totw;

			cpa->parent=cpa->pa[0];
		}
	}
}

void *exec_distribution(void *data)
{
	ParticleThread *thread= (ParticleThread*)data;
	ParticleSystem *psys= thread->ctx->psys;
	ParticleData *pa;
	ChildParticle *cpa;
	int p, totpart;

	if(thread->ctx->from == PART_FROM_CHILD) {
		totpart= psys->totchild;
		cpa= psys->child;

		for(p=0; p<totpart; p++, cpa++) {
			if(thread->ctx->skip) /* simplification skip */
				rng_skip(thread->rng, 5*thread->ctx->skip[p]);

			if((p+thread->num) % thread->tot == 0)
				psys_thread_distribute_particle(thread, NULL, cpa, p);
			else /* thread skip */
				rng_skip(thread->rng, 5);
		}
	}
	else {
		totpart= psys->totpart;
		pa= psys->particles + thread->num;
		for(p=thread->num; p<totpart; p+=thread->tot, pa+=thread->tot)
			psys_thread_distribute_particle(thread, pa, NULL, p);
	}

	return 0;
}

/* creates a distribution of coordinates on a DerivedMesh	*/
/*															*/
/* 1. lets check from what we are emitting					*/
/* 2. now we know that we have something to emit from so	*/
/*	  let's calculate some weights							*/
/* 2.1 from even distribution								*/
/* 2.2 and from vertex groups								*/
/* 3. next we determine the indexes of emitting thing that	*/
/*	  the particles will have								*/
/* 4. let's do jitter if we need it							*/
/* 5. now we're ready to set the indexes & distributions to	*/
/*	  the particles											*/
/* 6. and we're done!										*/

/* This is to denote functionality that does not yet work with mesh - only derived mesh */
int psys_threads_init_distribution(ParticleThread *threads, DerivedMesh *finaldm, int from)
{
	ParticleThreadContext *ctx= threads[0].ctx;
	Object *ob= ctx->ob;
	ParticleSystem *psys= ctx->psys;
	Object *tob;
	ParticleData *pa=0, *tpars= 0;
	ParticleSettings *part;
	ParticleSystem *tpsys;
	ParticleSeam *seams= 0;
	ChildParticle *cpa=0;
	KDTree *tree=0;
	DerivedMesh *dm= NULL;
	float *jit= NULL;
	int i, seed, p=0, totthread= threads[0].tot;
	int no_distr=0, cfrom=0;
	int tot=0, totpart, *index=0, children=0, totseam=0;
	//int *vertpart=0;
	int jitlevel= 1, distr;
	float *weight=0,*sum=0,*jitoff=0;
	float cur, maxweight=0.0, tweight, totweight, co[3], nor[3], orco[3], ornor[3];
	
	if(ob==0 || psys==0 || psys->part==0)
		return 0;

	part=psys->part;
	totpart=psys->totpart;
	if(totpart==0)
		return 0;

	if (!finaldm->deformedOnly && !CustomData_has_layer( &finaldm->faceData, CD_ORIGINDEX ) ) {
		error("Can't paint with the current modifier stack, disable destructive modifiers");
		return 0;
	}

	BLI_srandom(31415926 + psys->seed);
	
	if(from==PART_FROM_CHILD){
		distr=PART_DISTR_RAND;
		if(part->from!=PART_FROM_PARTICLE && part->childtype==PART_CHILD_FACES){
			dm= finaldm;
			children=1;

			tree=BLI_kdtree_new(totpart);

			for(p=0,pa=psys->particles; p<totpart; p++,pa++){
				psys_particle_on_dm(ob,dm,part->from,pa->num,pa->num_dmcache,pa->fuv,pa->foffset,co,nor,0,0,orco,ornor);
				transform_mesh_orco_verts((Mesh*)ob->data, &orco, 1, 1);
				BLI_kdtree_insert(tree, p, orco, ornor);
			}

			BLI_kdtree_balance(tree);

			totpart=get_alloc_child_particles_tot(psys);
			cfrom=from=PART_FROM_FACE;

			if(part->flag&PART_CHILD_SEAMS){
				MEdge *ed, *medge=dm->getEdgeDataArray(dm,CD_MEDGE);
				MVert *mvert=dm->getVertDataArray(dm,CD_MVERT);
				int totedge=dm->getNumEdges(dm);

				for(p=0, ed=medge; p<totedge; p++,ed++)
					if(ed->flag&ME_SEAM)
						totseam++;

				if(totseam){
					ParticleSeam *cur_seam=seams=MEM_callocN(totseam*sizeof(ParticleSeam),"Child Distribution Seams");
					float temp[3],temp2[3];

					for(p=0, ed=medge; p<totedge; p++,ed++){
						if(ed->flag&ME_SEAM){
							VecCopyf(cur_seam->v0,(mvert+ed->v1)->co);
							VecCopyf(cur_seam->v1,(mvert+ed->v2)->co);

							VecSubf(cur_seam->dir,cur_seam->v1,cur_seam->v0);

							cur_seam->length2=VecLength(cur_seam->dir);
							cur_seam->length2*=cur_seam->length2;

							temp[0]=(float)((mvert+ed->v1)->no[0]);
							temp[1]=(float)((mvert+ed->v1)->no[1]);
							temp[2]=(float)((mvert+ed->v1)->no[2]);
							temp2[0]=(float)((mvert+ed->v2)->no[0]);
							temp2[1]=(float)((mvert+ed->v2)->no[1]);
							temp2[2]=(float)((mvert+ed->v2)->no[2]);

							VecAddf(cur_seam->nor,temp,temp2);
							Normalize(cur_seam->nor);

							Crossf(cur_seam->tan,cur_seam->dir,cur_seam->nor);

							Normalize(cur_seam->tan);

							cur_seam++;
						}
					}
				}
				
			}
		}
		else{
			/* no need to figure out distribution */
			int child_nbr= (psys->renderdata)? part->ren_child_nbr: part->child_nbr;

			totpart= get_alloc_child_particles_tot(psys);
			alloc_child_particles(psys, totpart);
			cpa=psys->child;
			for(i=0; i<child_nbr; i++){
				for(p=0; p<psys->totpart; p++,cpa++){
					float length=2.0;
					cpa->parent=p;
					
					/* create even spherical distribution inside unit sphere */
					while(length>=1.0f){
						cpa->fuv[0]=2.0f*BLI_frand()-1.0f;
						cpa->fuv[1]=2.0f*BLI_frand()-1.0f;
						cpa->fuv[2]=2.0f*BLI_frand()-1.0f;
						length=VecLength(cpa->fuv);
					}

					cpa->rand[0]=BLI_frand();
					cpa->rand[1]=BLI_frand();
					cpa->rand[2]=BLI_frand();

					cpa->num=-1;
				}
			}

			return 0;
		}
	}
	else{
		dm= CDDM_from_mesh((Mesh*)ob->data, ob);

		/* special handling of grid distribution */
		if(part->distr==PART_DISTR_GRID){
			distribute_particles_in_grid(dm,psys);
			dm->release(dm);
			return 0;
		}

		/* we need orco for consistent distributions */
		DM_add_vert_layer(dm, CD_ORCO, CD_ASSIGN, get_mesh_orco_verts(ob));

		distr=part->distr;
		pa=psys->particles;
		if(from==PART_FROM_VERT){
			MVert *mv= dm->getVertDataArray(dm, CD_MVERT);
			float (*orcodata)[3]= dm->getVertDataArray(dm, CD_ORCO);
			int totvert = dm->getNumVerts(dm);

			tree=BLI_kdtree_new(totvert);

			for(p=0; p<totvert; p++){
				if(orcodata) {
					VECCOPY(co,orcodata[p])
					transform_mesh_orco_verts((Mesh*)ob->data, &co, 1, 1);
				}
				else
					VECCOPY(co,mv[p].co)
				BLI_kdtree_insert(tree,p,co,NULL);
			}

			BLI_kdtree_balance(tree);
		}
	}

	/* 1. */
	switch(from){
		case PART_FROM_VERT:
			tot = dm->getNumVerts(dm);
			break;
		case PART_FROM_VOLUME:
		case PART_FROM_FACE:
			tot = dm->getNumFaces(dm);
			break;
		case PART_FROM_PARTICLE:
			if(psys->target_ob)
				tob=psys->target_ob;
			else
				tob=ob;

			if((tpsys=BLI_findlink(&tob->particlesystem,psys->target_psys-1))){
				tpars=tpsys->particles;
				tot=tpsys->totpart;
			}
			break;
	}

	if(tot==0){
		no_distr=1;
		if(children){
			fprintf(stderr,"Particle child distribution error: Nothing to emit from!\n");
			for(p=0,cpa=psys->child; p<totpart; p++,cpa++){
				cpa->fuv[0]=cpa->fuv[1]=cpa->fuv[2]=cpa->fuv[3]= 0.0;
				cpa->foffset= 0.0f;
				cpa->parent=0;
				cpa->pa[0]=cpa->pa[1]=cpa->pa[2]=cpa->pa[3]=0;
				cpa->num= -1;
			}
		}
		else {
			fprintf(stderr,"Particle distribution error: Nothing to emit from!\n");
			for(p=0,pa=psys->particles; p<totpart; p++,pa++){
				pa->fuv[0]=pa->fuv[1]=pa->fuv[2]= pa->fuv[3]= 0.0;
				pa->foffset= 0.0f;
				pa->num= -1;
			}
		}

		if(dm != finaldm) dm->release(dm);
		return 0;
	}

	/* 2. */

	weight=MEM_callocN(sizeof(float)*tot, "particle_distribution_weights");
	index=MEM_callocN(sizeof(int)*totpart, "particle_distribution_indexes");
	sum=MEM_callocN(sizeof(float)*(tot+1), "particle_distribution_sum");
	jitoff=MEM_callocN(sizeof(float)*tot, "particle_distribution_jitoff");

	/* 2.1 */
	if((part->flag&PART_EDISTR || children) && ELEM(from,PART_FROM_PARTICLE,PART_FROM_VERT)==0){
		MVert *v1, *v2, *v3, *v4;
		float totarea=0.0, co1[3], co2[3], co3[3], co4[3];
		float (*orcodata)[3];
		
		orcodata= dm->getVertDataArray(dm, CD_ORCO);

		for(i=0; i<tot; i++){
			MFace *mf=dm->getFaceData(dm,i,CD_MFACE);

			if(orcodata) {
				VECCOPY(co1, orcodata[mf->v1]);
				VECCOPY(co2, orcodata[mf->v2]);
				VECCOPY(co3, orcodata[mf->v3]);
				transform_mesh_orco_verts((Mesh*)ob->data, &co1, 1, 1);
				transform_mesh_orco_verts((Mesh*)ob->data, &co2, 1, 1);
				transform_mesh_orco_verts((Mesh*)ob->data, &co3, 1, 1);
			}
			else {
				v1= (MVert*)dm->getVertData(dm,mf->v1,CD_MVERT);
				v2= (MVert*)dm->getVertData(dm,mf->v2,CD_MVERT);
				v3= (MVert*)dm->getVertData(dm,mf->v3,CD_MVERT);
				VECCOPY(co1, v1->co);
				VECCOPY(co2, v2->co);
				VECCOPY(co3, v3->co);
			}

			if (mf->v4){
				if(orcodata) {
					VECCOPY(co4, orcodata[mf->v4]);
					transform_mesh_orco_verts((Mesh*)ob->data, &co4, 1, 1);
				}
				else {
					v4= (MVert*)dm->getVertData(dm,mf->v4,CD_MVERT);
					VECCOPY(co4, v4->co);
				}
				cur= AreaQ3Dfl(co1, co2, co3, co4);
			}
			else
				cur= AreaT3Dfl(co1, co2, co3);
			
			if(cur>maxweight)
				maxweight=cur;

			weight[i]= cur;
			totarea+=cur;
		}

		for(i=0; i<tot; i++)
			weight[i] /= totarea;

		maxweight /= totarea;
	}
	else if(from==PART_FROM_PARTICLE){
		float val=(float)tot/(float)totpart;
		for(i=0; i<tot; i++)
			weight[i]=val;
		maxweight=val;
	}
	else{
		float min=1.0f/(float)(MIN2(tot,totpart));
		for(i=0; i<tot; i++)
			weight[i]=min;
		maxweight=min;
	}

	/* 2.2 */
	if(ELEM3(from,PART_FROM_VERT,PART_FROM_FACE,PART_FROM_VOLUME)){
		float *vweight= psys_cache_vgroup(dm,psys,PSYS_VG_DENSITY);

		if(vweight){
			if(from==PART_FROM_VERT) {
				for(i=0;i<tot; i++)
					weight[i]*=vweight[i];
			}
			else { /* PART_FROM_FACE / PART_FROM_VOLUME */
				for(i=0;i<tot; i++){
					MFace *mf=dm->getFaceData(dm,i,CD_MFACE);
					tweight = vweight[mf->v1] + vweight[mf->v2] + vweight[mf->v3];
				
					if(mf->v4) {
						tweight += vweight[mf->v4];
						tweight /= 4.0;
					}
					else {
						tweight /= 3.0;
					}

					weight[i]*=tweight;
				}
			}
			MEM_freeN(vweight);
		}
	}

	/* 3. */
	totweight= 0.0f;
	for(i=0;i<tot; i++)
		totweight += weight[i];

	if(totweight > 0.0f)
		totweight= 1.0f/totweight;

	sum[0]= 0.0f;
	for(i=0;i<tot; i++)
		sum[i+1]= sum[i]+weight[i]*totweight;
	
	if((part->flag&PART_TRAND) || (part->simplify_flag&PART_SIMPLIFY_ENABLE)) {
		float pos;

		for(p=0; p<totpart; p++) {
			pos= BLI_frand();
			index[p]= binary_search_distribution(sum, tot, pos);
			index[p]= MIN2(tot-1, index[p]);
			jitoff[index[p]]= pos;
		}
	}
	else {
		double step, pos;
		
		step= (totpart <= 1)? 0.5: 1.0/(totpart-1);
		pos= 0.0f;
		i= 0;

		for(p=0; p<totpart; p++, pos+=step) {
			while((i < tot) && (pos > sum[i+1]))
				i++;

			index[p]= MIN2(tot-1, i);
			if(p == totpart-1 && weight[index[p]] == 0.0f)
				index[p]= index[p-1];

			jitoff[index[p]]= pos;
		}
	}

	MEM_freeN(sum);

	/* weights are no longer used except for FROM_PARTICLE, which needs them zeroed for indexing */
	if(from==PART_FROM_PARTICLE){
		for(i=0; i<tot; i++)
			weight[i]=0.0f;
	}

	/* 4. */
	if(distr==PART_DISTR_JIT && ELEM(from,PART_FROM_FACE,PART_FROM_VOLUME)) {
		jitlevel= part->userjit;
		
		if(jitlevel == 0) {
			jitlevel= totpart/tot;
			if(part->flag & PART_EDISTR) jitlevel*= 2;	/* looks better in general, not very scietific */
			if(jitlevel<3) jitlevel= 3;
			//if(jitlevel>100) jitlevel= 100;
		}
		
		jit= MEM_callocN(2+ jitlevel*2*sizeof(float), "jit");

		init_mv_jit(jit, jitlevel, psys->seed, part->jitfac);
		BLI_array_randomize(jit, 2*sizeof(float), jitlevel, psys->seed); /* for custom jit or even distribution */
	}

	/* 5. */
	ctx->tree= tree;
	ctx->seams= seams;
	ctx->totseam= totseam;
	ctx->psys= psys;
	ctx->index= index;
	ctx->jit= jit;
	ctx->jitlevel= jitlevel;
	ctx->jitoff= jitoff;
	ctx->weight= weight;
	ctx->maxweight= maxweight;
	ctx->from= (children)? PART_FROM_CHILD: from;
	ctx->cfrom= cfrom;
	ctx->distr= distr;
	ctx->dm= dm;
	ctx->tpars= tpars;

	if(children) {
		totpart= psys_render_simplify_distribution(ctx, totpart);
		alloc_child_particles(psys, totpart);
	}

	if(!children || psys->totchild < 10000)
		totthread= 1;
	
	seed= 31415926 + ctx->psys->seed;
	for(i=0; i<totthread; i++) {
		threads[i].rng= rng_new(seed);
		threads[i].tot= totthread;
	}

	return 1;
}

static void distribute_particles_on_dm(DerivedMesh *finaldm, Object *ob, ParticleSystem *psys, int from)
{
	ListBase threads;
	ParticleThread *pthreads;
	ParticleThreadContext *ctx;
	int i, totthread;

	pthreads= psys_threads_create(ob, psys, G.scene->r.threads);

	if(!psys_threads_init_distribution(pthreads, finaldm, from)) {
		psys_threads_free(pthreads);
		return;
	}

	totthread= pthreads[0].tot;
	if(totthread > 1) {
		BLI_init_threads(&threads, exec_distribution, totthread);

		for(i=0; i<totthread; i++)
			BLI_insert_thread(&threads, &pthreads[i]);

		BLI_end_threads(&threads);
	}
	else
		exec_distribution(&pthreads[0]);

	if (from == PART_FROM_FACE)
		psys_calc_dmfaces(ob, finaldm, psys);

	ctx= pthreads[0].ctx;
	if(ctx->dm != finaldm)
		ctx->dm->release(ctx->dm);

	psys_threads_free(pthreads);
}

/* ready for future use, to emit particles without geometry */
static void distribute_particles_on_shape(Object *ob, ParticleSystem *psys, int from)
{
	ParticleData *pa;
	int totpart=psys->totpart, p;

	fprintf(stderr,"Shape emission not yet possible!\n");

	for(p=0,pa=psys->particles; p<totpart; p++,pa++){
		pa->fuv[0]=pa->fuv[1]=pa->fuv[2]=pa->fuv[3]= 0.0;
		pa->foffset= 0.0f;
		pa->num= -1;
	}
}
static void distribute_particles(Object *ob, ParticleSystem *psys, int from)
{
	ParticleSystemModifierData *psmd=0;
	int distr_error=0;
	psmd=psys_get_modifier(ob,psys);

	if(psmd){
		if(psmd->dm)
			distribute_particles_on_dm(psmd->dm,ob,psys,from);
		else
			distr_error=1;
	}
	else
		distribute_particles_on_shape(ob,psys,from);

	if(distr_error){
		ParticleData *pa;
		int totpart=psys->totpart, p;

		fprintf(stderr,"Particle distribution error!\n");

		for(p=0,pa=psys->particles; p<totpart; p++,pa++){
			pa->fuv[0]=pa->fuv[1]=pa->fuv[2]=pa->fuv[3]= 0.0;
			pa->foffset= 0.0f;
			pa->num= -1;
		}
	}
}

/* threaded child particle distribution and path caching */
ParticleThread *psys_threads_create(struct Object *ob, struct ParticleSystem *psys, int totthread)
{
	ParticleThread *threads;
	ParticleThreadContext *ctx;
	int i;
	
	threads= MEM_callocN(sizeof(ParticleThread)*totthread, "ParticleThread");
	ctx= MEM_callocN(sizeof(ParticleThreadContext), "ParticleThreadContext");

	ctx->ob= ob;
	ctx->psys= psys;
	ctx->psmd= psys_get_modifier(ob, psys);
	ctx->dm= ctx->psmd->dm;
	ctx->ma= give_current_material(ob, psys->part->omat);

	memset(threads, 0, sizeof(ParticleThread)*totthread);

	for(i=0; i<totthread; i++) {
		threads[i].ctx= ctx;
		threads[i].num= i;
		threads[i].tot= totthread;
	}

	return threads;
}

void psys_threads_free(ParticleThread *threads)
{
	ParticleThreadContext *ctx= threads[0].ctx;
	int i, totthread= threads[0].tot;

	/* path caching */
	if(ctx->vg_length)
		MEM_freeN(ctx->vg_length);
	if(ctx->vg_clump)
		MEM_freeN(ctx->vg_clump);
	if(ctx->vg_kink)
		MEM_freeN(ctx->vg_kink);
	if(ctx->vg_rough1)
		MEM_freeN(ctx->vg_rough1);
	if(ctx->vg_rough2)
		MEM_freeN(ctx->vg_roughe);
	if(ctx->vg_roughe)
		MEM_freeN(ctx->vg_roughe);

	if(ctx->psys->lattice){
		end_latt_deform();
		ctx->psys->lattice=0;
	}

	/* distribution */
	if(ctx->jit) MEM_freeN(ctx->jit);
	if(ctx->jitoff) MEM_freeN(ctx->jitoff);
	if(ctx->weight) MEM_freeN(ctx->weight);
	if(ctx->index) MEM_freeN(ctx->index);
	if(ctx->skip) MEM_freeN(ctx->skip);
	if(ctx->seams) MEM_freeN(ctx->seams);
	//if(ctx->vertpart) MEM_freeN(ctx->vertpart);
	BLI_kdtree_free(ctx->tree);

	/* threads */
	for(i=0; i<totthread; i++) {
		if(threads[i].rng)
			rng_free(threads[i].rng);
		if(threads[i].rng_path)
			rng_free(threads[i].rng_path);
	}

	MEM_freeN(ctx);
	MEM_freeN(threads);
}

/* set particle parameters that don't change during particle's life */
void initialize_particle(ParticleData *pa, int p, Object *ob, ParticleSystem *psys, ParticleSystemModifierData *psmd)
{
	ParticleSettings *part;
	ParticleTexture ptex;
	Material *ma=0;
	IpoCurve *icu=0;
	int totpart;
	float rand,length;

	part=psys->part;

	totpart=psys->totpart;

	ptex.life=ptex.size=ptex.exist=ptex.length=1.0;
	ptex.time=(float)p/(float)totpart;

	BLI_srandom(psys->seed+p);

	if(part->from!=PART_FROM_PARTICLE){
		ma=give_current_material(ob,part->omat);

		/* TODO: needs some work to make most blendtypes generally usefull */
		psys_get_texture(ob,ma,psmd,psys,pa,&ptex,MAP_PA_INIT);
	}
	
	pa->lifetime= part->lifetime*ptex.life;

	if(part->type==PART_HAIR)
		pa->time=0.0f;
	else if(part->type==PART_REACTOR && (part->flag&PART_REACT_STA_END)==0)
		pa->time=MAXFRAMEF;
	else{
		//icu=find_ipocurve(psys->part->ipo,PART_EMIT_TIME);
		//if(icu){
		//	calc_icu(icu,100*ptex.time);
		//	ptex.time=icu->curval;
		//}

		pa->time= part->sta + (part->end - part->sta)*ptex.time;
	}


	if(part->type==PART_HAIR){
		pa->lifetime=100.0f;
	}
	else{
		icu=find_ipocurve(psys->part->ipo,PART_EMIT_LIFE);
		if(icu){
			calc_icu(icu,100*ptex.time);
			pa->lifetime*=icu->curval;
		}

	/* need to get every rand even if we don't use them so that randoms don't affect eachother */
		rand= BLI_frand();
		if(part->randlife!=0.0)
			pa->lifetime*= 1.0f - part->randlife*rand;
	}

	pa->dietime= pa->time+pa->lifetime;

	pa->sizemul= BLI_frand();

	rand= BLI_frand();

	/* while loops are to have a spherical distribution (avoid cubic distribution) */
	length=2.0f;
	while(length>1.0){
		pa->r_ve[0]=2.0f*(BLI_frand()-0.5f);
		pa->r_ve[1]=2.0f*(BLI_frand()-0.5f);
		pa->r_ve[2]=2.0f*(BLI_frand()-0.5f);
		length=VecLength(pa->r_ve);
	}

	length=2.0f;
	while(length>1.0){
		pa->r_ave[0]=2.0f*(BLI_frand()-0.5f);
		pa->r_ave[1]=2.0f*(BLI_frand()-0.5f);
		pa->r_ave[2]=2.0f*(BLI_frand()-0.5f);
		length=VecLength(pa->r_ave);
	}

	pa->r_rot[0]=2.0f*(BLI_frand()-0.5f);
	pa->r_rot[1]=2.0f*(BLI_frand()-0.5f);
	pa->r_rot[2]=2.0f*(BLI_frand()-0.5f);
	pa->r_rot[3]=2.0f*(BLI_frand()-0.5f);

	NormalQuat(pa->r_rot);

	if(part->distr!=PART_DISTR_GRID){
		/* any unique random number will do (r_ave[0]) */
		if(ptex.exist < 0.5*(1.0+pa->r_ave[0]))
			pa->flag |= PARS_UNEXIST;
		else
			pa->flag &= ~PARS_UNEXIST;
	}

	pa->loop=0;
	/* we can't reset to -1 anymore since we've figured out correct index in distribute_particles */
	/* usage other than straight after distribute has to handle this index by itself - jahka*/
	//pa->num_dmcache = DMCACHE_NOTFOUND; /* assume we dont have a derived mesh face */
}
static void initialize_all_particles(Object *ob, ParticleSystem *psys, ParticleSystemModifierData *psmd)
{
	IpoCurve *icu=0;
	ParticleData *pa;
	int p, totpart=psys->totpart;

	for(p=0, pa=psys->particles; p<totpart; p++, pa++)
		initialize_particle(pa,p,ob,psys,psmd);
	
	/* store the derived mesh face index for each particle */
	icu=find_ipocurve(psys->part->ipo,PART_EMIT_FREQ);
	if(icu){
		float time=psys->part->sta, end=psys->part->end;
		float v1, v2, a=0.0f, t1,t2, d;

		p=0;
		pa=psys->particles;

		calc_icu(icu,time);
		v1=icu->curval;
		if(v1<0.0f) v1=0.0f;

		calc_icu(icu,time+1.0f);
		v2=icu->curval;
		if(v2<0.0f) v2=0.0f;

		for(p=0, pa=psys->particles; p<totpart && time<end; p++, pa++){
			while(a+0.5f*(v1+v2) < (float)(p+1) && time<end){
				a+=0.5f*(v1+v2);
				v1=v2;
				time++;
				calc_icu(icu,time+1.0f);
				v2=icu->curval;
			}
			if(time<end){
				if(v1==v2){
					pa->time=time+((float)(p+1)-a)/v1;
				}
				else{
					d=(float)sqrt(v1*v1-2.0f*(v2-v1)*(a-(float)(p+1)));
					t1=(-v1+d)/(v2-v1);
					t2=(-v1-d)/(v2-v1);

					/* the root between 0-1 is the correct one */
					if(t1>0.0f && t1<=1.0f)
						pa->time=time+t1;
					else
						pa->time=time+t2;
				}
			}

			pa->dietime = pa->time+pa->lifetime;
			pa->flag &= ~PARS_UNEXIST;
		}
		for(; p<totpart; p++, pa++){
			pa->flag |= PARS_UNEXIST;
		}
	}
}
/* sets particle to the emitter surface with initial velocity & rotation */
void reset_particle(ParticleData *pa, ParticleSystem *psys, ParticleSystemModifierData *psmd, Object *ob,
					float dtime, float cfra, float *vg_vel, float *vg_tan, float *vg_rot)
{
	ParticleSettings *part;
	ParticleTexture ptex;
	ParticleKey state;
	IpoCurve *icu=0;
	float fac, nor[3]={0,0,0},loc[3],tloc[3],vel[3]={0.0,0.0,0.0},rot[4],*q2=0;
	float r_vel[3],r_ave[3],r_rot[4],p_vel[3]={0.0,0.0,0.0};
	float x_vec[3]={1.0,0.0,0.0}, utan[3]={0.0,1.0,0.0}, vtan[3]={0.0,0.0,1.0};

	float q_one[4]={1.0,0.0,0.0,0.0}, q_phase[4];
	part=psys->part;

	ptex.ivel=1.0;
	
	if(part->from==PART_FROM_PARTICLE){
		Object *tob;
		ParticleSystem *tpsys=0;
		float speed;

		tob=psys->target_ob;
		if(tob==0)
			tob=ob;

		tpsys=BLI_findlink(&tob->particlesystem,psys->target_psys-1);
		
		/*TODO: get precise location of particle at birth*/

		state.time=cfra;
		psys_get_particle_state(tob,tpsys,pa->num,&state,1);
		psys_get_from_key(&state,loc,nor,rot,0);

		QuatMulVecf(rot,vtan);
		QuatMulVecf(rot,utan);
		VECCOPY(r_vel,pa->r_ve);
		VECCOPY(r_rot,pa->r_rot);
		VECCOPY(r_ave,pa->r_ave);

		VECCOPY(p_vel,state.vel);
		speed=Normalize(p_vel);
		VecMulf(p_vel,Inpf(pa->r_ve,p_vel));
		VECSUB(p_vel,pa->r_ve,p_vel);
		Normalize(p_vel);
		VecMulf(p_vel,speed);
	}
	else{
		/* get precise emitter matrix if particle is born */
		if(part->type!=PART_HAIR && pa->time < cfra && pa->time >= psys->cfra)
			where_is_object_time(ob,pa->time);

		/* get birth location from object		*/
		psys_particle_on_emitter(ob,psmd,part->from,pa->num, pa->num_dmcache, pa->fuv,pa->foffset,loc,nor,utan,vtan,0,0);
		
		/* save local coordinates for later		*/
		VECCOPY(tloc,loc);
		
		/* get possible textural influence */
		psys_get_texture(ob,give_current_material(ob,part->omat),psmd,psys,pa,&ptex,MAP_PA_IVEL);

		if(vg_vel){
			ptex.ivel*=psys_interpolate_value_from_verts(psmd->dm,part->from,pa->num,pa->fuv,vg_vel);
		}

		/* particles live in global space so	*/
		/* let's convert:						*/
		/* -location							*/
		Mat4MulVecfl(ob->obmat,loc);
		
		/* -normal								*/
		VECADD(nor,tloc,nor);
		Mat4MulVecfl(ob->obmat,nor);
		VECSUB(nor,nor,loc);
		Normalize(nor);

		/* -tangent								*/
		if(part->tanfac!=0.0){
			float phase=vg_rot?2.0f*(psys_interpolate_value_from_verts(psmd->dm,part->from,pa->num,pa->fuv,vg_rot)-0.5f):0.0f;
			VecMulf(vtan,-(float)cos(M_PI*(part->tanphase+phase)));
			fac=-(float)sin(M_PI*(part->tanphase+phase));
			VECADDFAC(vtan,vtan,utan,fac);

			VECADD(vtan,tloc,vtan);
			Mat4MulVecfl(ob->obmat,vtan);
			VECSUB(vtan,vtan,loc);

			VECCOPY(utan,nor);
			VecMulf(utan,Inpf(vtan,nor));
			VECSUB(vtan,vtan,utan);
			
			Normalize(vtan);
		}
		

		/* -velocity							*/
		if(part->randfac!=0.0){
			VECADD(r_vel,tloc,pa->r_ve);
			Mat4MulVecfl(ob->obmat,r_vel);
			VECSUB(r_vel,r_vel,loc);
			Normalize(r_vel);
		}

		/* -angular velocity					*/
		if(part->avemode==PART_AVE_RAND){
			VECADD(r_ave,tloc,pa->r_ave);
			Mat4MulVecfl(ob->obmat,r_ave);
			VECSUB(r_ave,r_ave,loc);
			Normalize(r_ave);
		}
		
		/* -rotation							*/
		if(part->rotmode==PART_ROT_RAND){
			QUATCOPY(r_rot,pa->r_rot);
			Mat4ToQuat(ob->obmat,rot);
			QuatMul(r_rot,r_rot,rot);
		}
	}
	/* conversion done so now we apply new:	*/
	/* -velocity from:						*/
	/*		*emitter velocity				*/
	if(dtime!=0.0 && part->obfac!=0.0){
		VECSUB(vel,loc,pa->state.co);
		VecMulf(vel,part->obfac/dtime);
	}
	
	/*		*emitter normal					*/
	if(part->normfac!=0.0)
		VECADDFAC(vel,vel,nor,part->normfac);
	
	/*		*emitter tangent				*/
	if(part->tanfac!=0.0)
		VECADDFAC(vel,vel,vtan,part->tanfac*(vg_tan?psys_interpolate_value_from_verts(psmd->dm,part->from,pa->num,pa->fuv,vg_tan):1.0f));

	/*		*texture						*/
	/* TODO	*/

	/*		*random							*/
	if(part->randfac!=0.0)
		VECADDFAC(vel,vel,r_vel,part->randfac);

	/*		*particle						*/
	if(part->partfac!=0.0)
		VECADDFAC(vel,vel,p_vel,part->partfac);

	icu=find_ipocurve(psys->part->ipo,PART_EMIT_VEL);
	if(icu){
		calc_icu(icu,100*((pa->time-part->sta)/(part->end-part->sta)));
		ptex.ivel*=icu->curval;
	}

	VecMulf(vel,ptex.ivel);
	
	VECCOPY(pa->state.vel,vel);

	/* -location from emitter				*/
	VECCOPY(pa->state.co,loc);

	/* -rotation							*/
	pa->state.rot[0]=1.0;
	pa->state.rot[1]=pa->state.rot[2]=pa->state.rot[3]=0.0;

	if(part->rotmode){
		switch(part->rotmode){
			case PART_ROT_NOR:
				VecMulf(nor,-1.0);
				q2= vectoquat(nor, OB_POSX, OB_POSZ);
				VecMulf(nor,-1.0);
				break;
			case PART_ROT_VEL:
				VecMulf(vel,-1.0);
				q2= vectoquat(vel, OB_POSX, OB_POSZ);
				VecMulf(vel,-1.0);
				break;
			case PART_ROT_RAND:
				q2= r_rot;
				break;
		}
		/* how much to rotate from rest position */
		QuatInterpol(rot,q_one,q2,part->rotfac);

		/* phase */
		VecRotToQuat(x_vec,part->phasefac*(float)M_PI,q_phase);

		/* combine amount & phase */
		QuatMul(pa->state.rot,rot,q_phase);
	}

	/* -angular velocity					*/

	pa->state.ave[0]=pa->state.ave[1]=pa->state.ave[2]=0.0;

	if(part->avemode){
		switch(part->avemode){
			case PART_AVE_SPIN:
				VECCOPY(pa->state.ave,vel);
				break;
			case PART_AVE_RAND:
				VECCOPY(pa->state.ave,r_ave);
				break;
		}
		Normalize(pa->state.ave);
		VecMulf(pa->state.ave,part->avefac);

		icu=find_ipocurve(psys->part->ipo,PART_EMIT_AVE);
		if(icu){
			calc_icu(icu,100*((pa->time-part->sta)/(part->end-part->sta)));
			VecMulf(pa->state.ave,icu->curval);
		}
	}

	pa->dietime=pa->time+pa->lifetime;

	if(pa->time >= cfra)
		pa->alive=PARS_UNBORN;

	pa->state.time=cfra;

	pa->stick_ob=0;
	pa->flag&=~PARS_STICKY;
}
static void reset_all_particles(Object *ob, ParticleSystem *psys, ParticleSystemModifierData *psmd, float dtime, float cfra, int from)
{
	ParticleData *pa;
	int p, totpart=psys->totpart;
	float *vg_vel=psys_cache_vgroup(psmd->dm,psys,PSYS_VG_VEL);
	float *vg_tan=psys_cache_vgroup(psmd->dm,psys,PSYS_VG_TAN);
	float *vg_rot=psys_cache_vgroup(psmd->dm,psys,PSYS_VG_ROT);
	
	for(p=from, pa=psys->particles+from; p<totpart; p++, pa++)
		reset_particle(pa, psys, psmd, ob, dtime, cfra, vg_vel, vg_tan, vg_rot);

	if(vg_vel)
		MEM_freeN(vg_vel);
}
/************************************************/
/*			Keyed particles						*/
/************************************************/
/* a bit of an unintuitive function :) counts objects in a keyed chain and returns 1 if some of them were selected (used in drawing) */
int psys_count_keyed_targets(Object *ob, ParticleSystem *psys)
{
	ParticleSystem *kpsys=psys,*tpsys;
	ParticleSettings *tpart;
	Object *kob=ob,*tob;
	int select=ob->flag&SELECT;
	short totkeyed=0;
	Base *base;

	ListBase lb;
	lb.first=lb.last=0;

	tob=psys->keyed_ob;
	while(tob){
		if((tpsys=BLI_findlink(&tob->particlesystem,kpsys->keyed_psys-1))){
			tpart=tpsys->part;

			if(tpart->phystype==PART_PHYS_KEYED){
				if(lb.first){
					for(base=lb.first;base;base=base->next){
						if(tob==base->object){
							fprintf(stderr,"Error: loop in keyed chain!\n");
							BLI_freelistN(&lb);
							return select;
						}
					}
				}
				base=MEM_callocN(sizeof(Base), "keyed base");
				base->object=tob;
				BLI_addtail(&lb,base);

				if(tob->flag&SELECT)
					select++;
				kob=tob;
				kpsys=tpsys;
				tob=tpsys->keyed_ob;
				totkeyed++;
			}
			else{
				tob=0;
				totkeyed++;
			}
		}
		else
			tob=0;
	}
	psys->totkeyed=totkeyed;
	BLI_freelistN(&lb);
	return select;
}
void set_keyed_keys(Object *ob, ParticleSystem *psys)
{
	Object *kob = ob;
	ParticleSystem *kpsys = psys;
	ParticleData *pa;
	ParticleKey state;
	int totpart = psys->totpart, i, k, totkeys = psys->totkeyed + 1;
	float prevtime, nexttime, keyedtime;

	/* no proper targets so let's clear and bail out */
	if(psys->totkeyed==0){
		free_keyed_keys(psys);
		psys->flag &= ~PSYS_KEYED;
		return;
	}

	if(totpart && psys->particles->totkey != totkeys){
		free_keyed_keys(psys);
		
		psys->particles->keys = MEM_callocN(psys->totpart * totkeys * sizeof(ParticleKey),"Keyed keys");

		psys->particles->totkey = totkeys;
		
		for(i=1, pa=psys->particles+1; i<totpart; i++,pa++){
			pa->keys = (pa-1)->keys + totkeys;
			pa->totkey = totkeys;
		}
	}
	
	psys->flag &= ~PSYS_KEYED;
	state.time=-1.0;

	for(k=0; k<totkeys; k++){
		for(i=0,pa=psys->particles; i<totpart; i++, pa++){
			psys_get_particle_state(kob, kpsys, i%kpsys->totpart, pa->keys + k, 1);

			if(k==0)
				pa->keys->time = pa->time;
			else if(k==totkeys-1)
				(pa->keys + k)->time = pa->time + pa->lifetime;
			else{
				if(psys->flag & PSYS_KEYED_TIME){
					prevtime = (pa->keys + k - 1)->time;
					nexttime = pa->time + pa->lifetime;
					keyedtime = kpsys->part->keyed_time;
					(pa->keys + k)->time = (1.0f - keyedtime) * prevtime + keyedtime * nexttime;
				}
				else
					(pa->keys+k)->time = pa->time + (float)k / (float)(totkeys - 1) * pa->lifetime;
			}
		}
		if(kpsys->keyed_ob){
			kob = kpsys->keyed_ob;
			kpsys = BLI_findlink(&kob->particlesystem, kpsys->keyed_psys - 1);
		}
	}

	psys->flag |= PSYS_KEYED;
}
/************************************************/
/*			Reactors							*/
/************************************************/
static void push_reaction(Object* ob, ParticleSystem *psys, int pa_num, int event, ParticleKey *state)
{
	Object *rob;
	ParticleSystem *rpsys;
	ParticleSettings *rpart;
	ParticleData *pa;
	ListBase *lb=&psys->effectors;
	ParticleEffectorCache *ec;
	ParticleReactEvent *re;

	if(lb->first) for(ec = lb->first; ec; ec= ec->next){
		if(ec->type & PSYS_EC_REACTOR){
			/* all validity checks already done in add_to_effectors */
			rob=ec->ob;
			rpsys=BLI_findlink(&rob->particlesystem,ec->psys_nbr);
			rpart=rpsys->part;
			if(rpsys->part->reactevent==event){
				pa=psys->particles+pa_num;
				re= MEM_callocN(sizeof(ParticleReactEvent), "react event");
				re->event=event;
				re->pa_num = pa_num;
				re->ob = ob;
				re->psys = psys;
				re->size = pa->size;
				copy_particle_key(&re->state,state,1);

				switch(event){
					case PART_EVENT_DEATH:
						re->time=pa->dietime;
						break;
					case PART_EVENT_COLLIDE:
						re->time=state->time;
						break;
					case PART_EVENT_NEAR:
						re->time=state->time;
						break;
				}

				BLI_addtail(&rpsys->reactevents, re);
			}
		}
	}
}
static void react_to_events(ParticleSystem *psys, int pa_num)
{
	ParticleSettings *part=psys->part;
	ParticleData *pa=psys->particles+pa_num;
	ParticleReactEvent *re=psys->reactevents.first;
	int birth=0;
	float dist=0.0f;

	for(re=psys->reactevents.first; re; re=re->next){
		birth=0;
		if(part->from==PART_FROM_PARTICLE){
			if(pa->num==re->pa_num){
				if(re->event==PART_EVENT_NEAR){
					ParticleData *tpa = re->psys->particles+re->pa_num;
					float pa_time=tpa->time + pa->foffset*tpa->lifetime;
					if(re->time > pa_time){
						pa->alive=PARS_ALIVE;
						pa->time=pa_time;
						pa->dietime=pa->time+pa->lifetime;
					}
				}
				else{
					if(pa->alive==PARS_UNBORN){
						pa->alive=PARS_ALIVE;
						pa->time=re->time;
						pa->dietime=pa->time+pa->lifetime;
					}
				}
			}
		}
		else{
			dist=VecLenf(pa->state.co, re->state.co);
			if(dist <= re->size){
				if(pa->alive==PARS_UNBORN){
					pa->alive=PARS_ALIVE;
					pa->time=re->time;
					pa->dietime=pa->time+pa->lifetime;
					birth=1;
				}
				if(birth || part->flag&PART_REACT_MULTIPLE){
					float vec[3];
					VECSUB(vec,pa->state.co, re->state.co);
					if(birth==0)
						VecMulf(vec,(float)pow(1.0f-dist/re->size,part->reactshape));
					VECADDFAC(pa->state.vel,pa->state.vel,vec,part->reactfac);
					VECADDFAC(pa->state.vel,pa->state.vel,re->state.vel,part->partfac);
				}
				if(birth)
					VecMulf(pa->state.vel,(float)pow(1.0f-dist/re->size,part->reactshape));
			}
		}
	}
}
void psys_get_reactor_target(Object *ob, ParticleSystem *psys, Object **target_ob, ParticleSystem **target_psys)
{
	Object *tob;

	tob=psys->target_ob;
	if(tob==0)
		tob=ob;
	
	*target_psys=BLI_findlink(&tob->particlesystem,psys->target_psys-1);
	if(*target_psys)
		*target_ob=tob;
	else
		*target_ob=0;
}
/************************************************/
/*			Point Cache							*/
/************************************************/
void clear_particles_from_cache(Object *ob, ParticleSystem *psys, int cfra)
{
	ParticleSystemModifierData *psmd = psys_get_modifier(ob,psys);
	int stack_index = modifiers_indexInObject(ob,(ModifierData*)psmd);

	BKE_ptcache_id_clear((ID *)ob, PTCACHE_CLEAR_ALL, cfra, stack_index);
}
static void write_particles_to_cache(Object *ob, ParticleSystem *psys, int cfra)
{
	FILE *fp = NULL;
	ParticleSystemModifierData *psmd = psys_get_modifier(ob,psys);
	ParticleData *pa;
	int stack_index = modifiers_indexInObject(ob,(ModifierData*)psmd);
	int i, totpart = psys->totpart;

	if(totpart == 0) return;

	fp = BKE_ptcache_id_fopen((ID *)ob, 'w', cfra, stack_index);
	if(!fp) return;

	for(i=0, pa=psys->particles; i<totpart; i++, pa++)
		fwrite(&pa->state, sizeof(ParticleKey), 1, fp);
	
	fclose(fp);
}
static int get_particles_from_cache(Object *ob, ParticleSystem *psys, int cfra)
{
	FILE *fp = NULL;
	ParticleSystemModifierData *psmd = psys_get_modifier(ob,psys);
	ParticleData *pa;
	int stack_index = modifiers_indexInObject(ob,(ModifierData*)psmd);
	int i, totpart = psys->totpart, ret = 1;

	if(totpart == 0) return 0;

	fp = BKE_ptcache_id_fopen((ID *)ob, 'r', cfra, stack_index);
	if(!fp)
		ret = 0;
	else {
		for(i=0, pa=psys->particles; i<totpart; i++, pa++)
			if((fread(&pa->state, sizeof(ParticleKey), 1, fp)) != 1) {
				ret = 0;
				break;
			}
		
		fclose(fp);
	}

	return ret;
}
/************************************************/
/*			Effectors							*/
/************************************************/
static float effector_falloff(PartDeflect *pd, float *eff_velocity, float *vec_to_part)
{
	float eff_dir[3], temp[3];
	float falloff=1.0, fac, r_fac;
	
	VecCopyf(eff_dir,eff_velocity);
	Normalize(eff_dir);

	if(pd->flag & PFIELD_POSZ && Inpf(eff_dir,vec_to_part)<0.0f)
		falloff=0.0f;
	else switch(pd->falloff){
		case PFIELD_FALL_SPHERE:
			fac=VecLength(vec_to_part);
			if(pd->flag&PFIELD_USEMAX && fac>pd->maxdist){
				falloff=0.0f;
				break;
			}

			if(pd->flag & PFIELD_USEMIN){
				if(fac>pd->mindist)
					fac+=1.0f-pd->mindist;
				else
					fac=1.0f;
			}
			else if(fac<0.001)
				fac=0.001f;

			falloff=1.0f/(float)pow((double)fac,(double)pd->f_power);
			break;

		case PFIELD_FALL_TUBE:
			fac=Inpf(vec_to_part,eff_dir);
			if(pd->flag&PFIELD_USEMAX && ABS(fac)>pd->maxdist){
				falloff=0.0f;
				break;
			}

			VECADDFAC(temp,vec_to_part,eff_dir,-fac);
			r_fac=VecLength(temp);
			if(pd->flag&PFIELD_USEMAXR && r_fac>pd->maxrad){
				falloff=0.0f;
				break;
			}

			fac=ABS(fac);


			if(pd->flag & PFIELD_USEMIN){
				if(fac>pd->mindist)
					fac+=1.0f-pd->mindist;
				else
					fac=1.0f;
			}
			else if(fac<0.001)
				fac=0.001f;

			if(pd->flag & PFIELD_USEMINR){
				if(r_fac>pd->minrad)
					r_fac+=1.0f-pd->minrad;
				else
					r_fac=1.0f;
			}
			else if(r_fac<0.001)
				r_fac=0.001f;

			falloff=1.0f/((float)pow((double)fac,(double)pd->f_power)*(float)pow((double)r_fac,(double)pd->f_power_r));

			break;
		case PFIELD_FALL_CONE:
			fac=Inpf(vec_to_part,eff_dir);
			if(pd->flag&PFIELD_USEMAX && ABS(fac)>pd->maxdist){
				falloff=0.0f;
				break;
			}
			r_fac=saacos(fac/VecLength(vec_to_part))*180.0f/(float)M_PI;
			if(pd->flag&PFIELD_USEMAXR && r_fac>pd->maxrad){
				falloff=0.0f;
				break;
			}

			if(pd->flag & PFIELD_USEMIN){
				if(fac>pd->mindist)
					fac+=1.0f-pd->mindist;
				else
					fac=1.0f;
			}
			else if(fac<0.001)
				fac=0.001f;

			if(pd->flag & PFIELD_USEMINR){
				if(r_fac>pd->minrad)
					r_fac+=1.0f-pd->minrad;
				else
					r_fac=1.0f;
			}
			else if(r_fac<0.001)
				r_fac=0.001f;

			falloff=1.0f/((float)pow((double)fac,(double)pd->f_power)*(float)pow((double)r_fac,(double)pd->f_power_r));

			break;
//		case PFIELD_FALL_INSIDE:
				//for(i=0; i<totface; i++,mface++){
				//	VECCOPY(v1,mvert[mface->v1].co);
				//	VECCOPY(v2,mvert[mface->v2].co);
				//	VECCOPY(v3,mvert[mface->v3].co);

				//	if(AxialLineIntersectsTriangle(a,co1, co2, v2, v3, v1, &lambda)){
				//		if(from==PART_FROM_FACE)
				//			(pa+(int)(lambda*size[a])*a0mul)->flag &= ~PARS_UNEXIST;
				//		else /* store number of intersections */
				//			(pa+(int)(lambda*size[a])*a0mul)->loop++;
				//	}
				//	
				//	if(mface->v4){
				//		VECCOPY(v4,mvert[mface->v4].co);

				//		if(AxialLineIntersectsTriangle(a,co1, co2, v4, v1, v3, &lambda)){
				//			if(from==PART_FROM_FACE)
				//				(pa+(int)(lambda*size[a])*a0mul)->flag &= ~PARS_UNEXIST;
				//			else
				//				(pa+(int)(lambda*size[a])*a0mul)->loop++;
				//		}
				//	}
				//}

//			break;
	}

	return falloff;
}
static void do_physical_effector(short type, float force_val, float distance, float falloff, float size, float damp,
							float *eff_velocity, float *vec_to_part, float *velocity, float *field, int planar)
{
	float mag_vec[3]={0,0,0};
	float temp[3], temp2[3];
	float eff_vel[3];

	VecCopyf(eff_vel,eff_velocity);
	Normalize(eff_vel);

	switch(type){
		case PFIELD_WIND:
			VECCOPY(mag_vec,eff_vel);

			VecMulf(mag_vec,force_val*falloff);
			VecAddf(field,field,mag_vec);
			break;

		case PFIELD_FORCE:
			if(planar)
				Projf(mag_vec,vec_to_part,eff_vel);
			else
				VecCopyf(mag_vec,vec_to_part);

			VecMulf(mag_vec,force_val*falloff);
			VecAddf(field,field,mag_vec);
			break;

		case PFIELD_VORTEX:
			Crossf(mag_vec,eff_vel,vec_to_part);
			Normalize(mag_vec);

			VecMulf(mag_vec,force_val*distance*falloff);
			VecAddf(field,field,mag_vec);

			break;
		case PFIELD_MAGNET:
			if(planar)
				VecCopyf(temp,eff_vel);
			else
				/* magnetic field of a moving charge */
				Crossf(temp,eff_vel,vec_to_part);

			Crossf(temp2,velocity,temp);
			VecAddf(mag_vec,mag_vec,temp2);

			VecMulf(mag_vec,force_val*falloff);
			VecAddf(field,field,mag_vec);
			break;
		case PFIELD_HARMONIC:
			if(planar)
				Projf(mag_vec,vec_to_part,eff_vel);
			else
				VecCopyf(mag_vec,vec_to_part);

			VecMulf(mag_vec,force_val*falloff);
			VecSubf(field,field,mag_vec);

			VecCopyf(mag_vec,velocity);
			/* 1.9 is an experimental value to get critical damping at damp=1.0 */
			VecMulf(mag_vec,damp*1.9f*(float)sqrt(force_val));
			VecSubf(field,field,mag_vec);
			break;
		case PFIELD_NUCLEAR:
			/*pow here is root of cosine expression below*/
			//rad=(float)pow(2.0,-1.0/power)*distance/size;
			//VECCOPY(mag_vec,vec_to_part);
			//Normalize(mag_vec);
			//VecMulf(mag_vec,(float)cos(3.0*M_PI/2.0*(1.0-1.0/(pow(rad,power)+1.0)))/(rad+0.2f));
			//VECADDFAC(field,field,mag_vec,force_val);
			break;
	}
}
static void do_texture_effector(Tex *tex, short mode, short is_2d, float nabla, short object, float *pa_co, float obmat[4][4], float force_val, float falloff, float *field)
{
	TexResult result[4];
	float tex_co[3], strength, mag_vec[3];
	int i;

	if(tex==0) return;

	for(i=0; i<4; i++)
		result[i].nor=0;

	strength= force_val*falloff;///(float)pow((double)distance,(double)power);

	VECCOPY(tex_co,pa_co);

	if(is_2d){
		float fac=-Inpf(tex_co,obmat[2]);
		VECADDFAC(tex_co,tex_co,obmat[2],fac);
	}

	if(object){
		VecSubf(tex_co,tex_co,obmat[3]);
		Mat4Mul3Vecfl(obmat,tex_co);
	}

	multitex_ext(tex, tex_co, NULL,NULL, 1, result);

	if(mode==PFIELD_TEX_RGB){
		mag_vec[0]= (0.5f-result->tr)*strength;
		mag_vec[1]= (0.5f-result->tg)*strength;
		mag_vec[2]= (0.5f-result->tb)*strength;
	}
	else{
		strength/=nabla;

		tex_co[0]+= nabla;
		multitex_ext(tex, tex_co, NULL,NULL, 1, result+1);

		tex_co[0]-= nabla;
		tex_co[1]+= nabla;
		multitex_ext(tex, tex_co, NULL,NULL, 1, result+2);

		tex_co[1]-= nabla;
		tex_co[2]+= nabla;
		multitex_ext(tex, tex_co, NULL,NULL, 1, result+3);

		if(mode==PFIELD_TEX_GRAD){
			mag_vec[0]= (result[0].tin-result[1].tin)*strength;
			mag_vec[1]= (result[0].tin-result[2].tin)*strength;
			mag_vec[2]= (result[0].tin-result[3].tin)*strength;
		}
		else{ /*PFIELD_TEX_CURL*/
			float dbdy,dgdz,drdz,dbdx,dgdx,drdy;

			dbdy= result[2].tb-result[0].tb;
			dgdz= result[3].tg-result[0].tg;
			drdz= result[3].tr-result[0].tr;
			dbdx= result[1].tb-result[0].tb;
			dgdx= result[1].tg-result[0].tg;
			drdy= result[2].tr-result[0].tr;

			mag_vec[0]=(dbdy-dgdz)*strength;
			mag_vec[1]=(drdz-dbdx)*strength;
			mag_vec[2]=(dgdx-drdy)*strength;
		}
	}

	if(is_2d){
		float fac=-Inpf(mag_vec,obmat[2]);
		VECADDFAC(mag_vec,mag_vec,obmat[2],fac);
	}

	VecAddf(field,field,mag_vec);
}
static void add_to_effectors(ListBase *lb, Object *ob, Object *obsrc, ParticleSystem *psys)
{
	ParticleEffectorCache *ec;
	PartDeflect *pd= ob->pd;
	short type=0,i;

	if(pd && ob != obsrc){
		if(pd->forcefield == PFIELD_GUIDE) {
			if(ob->type==OB_CURVE) {
				Curve *cu= ob->data;
				if(cu->flag & CU_PATH) {
					if(cu->path==NULL || cu->path->data==NULL)
						makeDispListCurveTypes(ob, 0);
					if(cu->path && cu->path->data) {
						type |= PSYS_EC_EFFECTOR;
					}
				}
			}
		}
		else if(pd->forcefield)
			type |= PSYS_EC_EFFECTOR;
	}
	
	if(pd && pd->deflect)
		type |= PSYS_EC_DEFLECT;

	if(type){
		ec= MEM_callocN(sizeof(ParticleEffectorCache), "effector cache");
		ec->ob= ob;
		ec->type=type;
		ec->distances=0;
		ec->locations=0;
		BLI_addtail(lb, ec);
	}

	type=0;

	/* add particles as different effectors */
	if(ob->particlesystem.first){
		ParticleSystem *epsys=ob->particlesystem.first;
		ParticleSettings *epart=0;
		Object *tob;

		for(i=0; epsys; epsys=epsys->next,i++){
			type=0;
			if(epsys!=psys){
				epart=epsys->part;

				if(epsys->part->pd && epsys->part->pd->forcefield)
					type=PSYS_EC_PARTICLE;

				if(epart->type==PART_REACTOR) {
					tob=epsys->target_ob;
					if(tob==0)
						tob=ob;
					if(BLI_findlink(&tob->particlesystem,epsys->target_psys-1)==psys)
						type|=PSYS_EC_REACTOR;
				}

				if(type){
					ec= MEM_callocN(sizeof(ParticleEffectorCache), "effector cache");
					ec->ob= ob;
					ec->type=type;
					ec->psys_nbr=i;
					BLI_addtail(lb, ec);
				}
			}
		}
				
	}
}
void psys_init_effectors(Object *obsrc, Group *group, ParticleSystem *psys)
{
	ListBase *listb=&psys->effectors;
	Base *base;
	unsigned int layer= obsrc->lay;

	listb->first=listb->last=0;
	
	if(group) {
		GroupObject *go;
		
		for(go= group->gobject.first; go; go= go->next) {
			if( (go->ob->lay & layer) && (go->ob->pd || go->ob->particlesystem.first)) {
				add_to_effectors(listb, go->ob, obsrc, psys);
			}
		}
	}
	else {
		for(base = G.scene->base.first; base; base= base->next) {
			if( (base->lay & layer) && (base->object->pd || base->object->particlesystem.first)) {
				add_to_effectors(listb, base->object, obsrc, psys);
			}
		}
	}
}

void psys_end_effectors(ParticleSystem *psys)
{
	ListBase *lb=&psys->effectors;
	if(lb->first) {
		ParticleEffectorCache *ec;
		for(ec= lb->first; ec; ec= ec->next){
			if(ec->distances)
				MEM_freeN(ec->distances);

			if(ec->locations)
				MEM_freeN(ec->locations);

			if(ec->face_minmax)
				MEM_freeN(ec->face_minmax);

			if(ec->vert_cos)
				MEM_freeN(ec->vert_cos);

			if(ec->tree)
				BLI_kdtree_free(ec->tree);
		}

		BLI_freelistN(lb);
	}
}

static void precalc_effectors(Object *ob, ParticleSystem *psys, ParticleSystemModifierData *psmd)
{
	ListBase *lb=&psys->effectors;
	ParticleEffectorCache *ec;
	ParticleSettings *part=psys->part;
	ParticleData *pa;
	float vec2[3],loc[3],*co=0;
	int p,totpart,totvert;
	
	for(ec= lb->first; ec; ec= ec->next) {
		PartDeflect *pd= ec->ob->pd;
		
		if(ec->type==PSYS_EC_EFFECTOR && pd->forcefield==PFIELD_GUIDE && ec->ob->type==OB_CURVE 
			&& part->phystype!=PART_PHYS_BOIDS) {
			float vec[4];

			where_on_path(ec->ob, 0.0, vec, vec2);

			Mat4MulVecfl(ec->ob->obmat,vec);
			Mat4Mul3Vecfl(ec->ob->obmat,vec2);

			QUATCOPY(ec->firstloc,vec);
			VECCOPY(ec->firstdir,vec2);

			totpart=psys->totpart;

			if(totpart){
				ec->distances=MEM_callocN(totpart*sizeof(float),"particle distances");
				ec->locations=MEM_callocN(totpart*3*sizeof(float),"particle locations");

				for(p=0,pa=psys->particles; p<totpart; p++, pa++){
					psys_particle_on_emitter(ob,psmd,part->from,pa->num,pa->num_dmcache,pa->fuv,pa->foffset,loc,0,0,0,0,0);
					Mat4MulVecfl(ob->obmat,loc);
					ec->distances[p]=VecLenf(loc,vec);
					VECSUB(loc,loc,vec);
					VECCOPY(ec->locations+3*p,loc);
				}
			}
		}
		else if(ec->type==PSYS_EC_DEFLECT){
			DerivedMesh *dm;
			MFace *mface=0;
			MVert *mvert=0;
			int i, totface;
			float v1[3],v2[3],v3[3],v4[4], *min, *max;

			if(ob==ec->ob)
				dm=psmd->dm;
			else{
				psys_disable_all(ec->ob);

				dm=mesh_get_derived_final(ec->ob,0);
				
				psys_enable_all(ec->ob);
			}

			if(dm){
				totvert=dm->getNumVerts(dm);
				totface=dm->getNumFaces(dm);
				mface=dm->getFaceDataArray(dm,CD_MFACE);
				mvert=dm->getVertDataArray(dm,CD_MVERT);

				/* Decide which is faster to calculate by the amount of*/
				/* matrice multiplications needed to convert spaces. */
				/* With size deflect we have to convert allways because */
				/* the object can be scaled nonuniformly (sphere->ellipsoid). */
				if(totvert<2*psys->totpart || part->flag & PART_SIZE_DEFL){
					co=ec->vert_cos=MEM_callocN(sizeof(float)*3*totvert,"Particle deflection vert cos");
					/* convert vert coordinates to global (particle) coordinates */
					for(i=0; i<totvert; i++, co+=3){
						VECCOPY(co,mvert[i].co);
						Mat4MulVecfl(ec->ob->obmat,co);
					}
					co=ec->vert_cos;
				}
				else
					ec->vert_cos=0;

				INIT_MINMAX(ec->ob_minmax,ec->ob_minmax+3);

				min=ec->face_minmax=MEM_callocN(sizeof(float)*6*totface,"Particle deflection face minmax");
				max=min+3;

				for(i=0; i<totface; i++,mface++,min+=6,max+=6){
					if(co){
						VECCOPY(v1,co+3*mface->v1);
						VECCOPY(v2,co+3*mface->v2);
						VECCOPY(v3,co+3*mface->v3);
					}
					else{
						VECCOPY(v1,mvert[mface->v1].co);
						VECCOPY(v2,mvert[mface->v2].co);
						VECCOPY(v3,mvert[mface->v3].co);
					}
					INIT_MINMAX(min,max);
					DO_MINMAX(v1,min,max);
					DO_MINMAX(v2,min,max);
					DO_MINMAX(v3,min,max);

					if(mface->v4){
						if(co){
							VECCOPY(v4,co+3*mface->v4);
						}
						else{
							VECCOPY(v4,mvert[mface->v4].co);
						}
						DO_MINMAX(v4,min,max);
					}

					DO_MINMAX(min,ec->ob_minmax,ec->ob_minmax+3);
					DO_MINMAX(max,ec->ob_minmax,ec->ob_minmax+3);
				}
			}
			else
				ec->face_minmax=0;
		}
		else if(ec->type==PSYS_EC_PARTICLE){
			if(psys->part->phystype==PART_PHYS_BOIDS){
				Object *eob = ec->ob;
				ParticleSystem *epsys;
				ParticleSettings *epart;
				ParticleData *epa;
				ParticleKey state;
				PartDeflect *pd;
				int totepart, p;
				epsys= BLI_findlink(&eob->particlesystem,ec->psys_nbr);
				epart= epsys->part;
				pd= epart->pd;
				totepart= epsys->totpart;
				if(pd->forcefield==PFIELD_FORCE && totepart){
					KDTree *tree;

					tree=BLI_kdtree_new(totepart);
					ec->tree=tree;

					for(p=0, epa=epsys->particles; p<totepart; p++,epa++)
						if(epa->alive==PARS_ALIVE && psys_get_particle_state(eob,epsys,p,&state,0))
							BLI_kdtree_insert(tree, p, state.co, NULL);

					BLI_kdtree_balance(tree);
				}
			}
		}
	}
}


/* calculate forces that all effectors apply to a particle*/
static void do_effectors(int pa_no, ParticleData *pa, ParticleKey *state, Object *ob, ParticleSystem *psys, float *force_field, float *vel,float framestep, float cfra)
{
	Object *eob;
	ParticleSystem *epsys;
	ParticleSettings *epart;
	ParticleData *epa;
	ParticleKey estate;
	PartDeflect *pd;
	ListBase *lb=&psys->effectors;
	ParticleEffectorCache *ec;
	float distance, vec_to_part[3];
	float falloff;
	int p;

	/* check all effector objects for interaction */
	if(lb->first){
		for(ec = lb->first; ec; ec= ec->next){
			eob= ec->ob;
			if(ec->type & PSYS_EC_EFFECTOR){
				pd=eob->pd;
				if(psys->part->type!=PART_HAIR && psys->part->integrator)
					where_is_object_time(eob,cfra);
				/* Get IPO force strength and fall off values here */
				//if (has_ipo_code(eob->ipo, OB_PD_FSTR))
				//	force_val = IPO_GetFloatValue(eob->ipo, OB_PD_FSTR, cfra);
				//else 
				//	force_val = pd->f_strength;
				
				//if (has_ipo_code(eob->ipo, OB_PD_FFALL)) 
				//	ffall_val = IPO_GetFloatValue(eob->ipo, OB_PD_FFALL, cfra);
				//else 
				//	ffall_val = pd->f_power;

				//if (has_ipo_code(eob->ipo, OB_PD_FMAXD)) 
				//	maxdist = IPO_GetFloatValue(eob->ipo, OB_PD_FMAXD, cfra);
				//else 
				//	maxdist = pd->maxdist;

				/* use center of object for distance calculus */
				//obloc= eob->obmat[3];
				VecSubf(vec_to_part, state->co, eob->obmat[3]);
				distance = VecLength(vec_to_part);

				falloff=effector_falloff(pd,eob->obmat[2],vec_to_part);

				if(falloff<=0.0f)
					;	/* don't do anything */
				else if(pd->forcefield==PFIELD_TEXTURE)
					do_texture_effector(pd->tex, pd->tex_mode, pd->flag&PFIELD_TEX_2D, pd->tex_nabla,
										pd->flag & PFIELD_TEX_OBJECT, state->co, eob->obmat,
										pd->f_strength, falloff, force_field);
				else
					do_physical_effector(pd->forcefield,pd->f_strength,distance,
										falloff,pd->f_dist,pd->f_damp,eob->obmat[2],vec_to_part,
										pa->state.vel,force_field,pd->flag&PFIELD_PLANAR);
			}
			if(ec->type & PSYS_EC_PARTICLE){
				int totepart;
				epsys= BLI_findlink(&eob->particlesystem,ec->psys_nbr);
				epart= epsys->part;
				pd= epart->pd;
				totepart= epsys->totpart;

				if(pd->forcefield==PFIELD_HARMONIC){
					/* every particle is mapped to only one harmonic effector particle */
					p= pa_no%epsys->totpart;
					totepart= p+1;
				}
				else{
					p=0;
				}

				epsys->lattice=psys_get_lattice(ob,psys);

				for(; p<totepart; p++){
					epa = epsys->particles + p;
					estate.time=-1.0;
					if(psys_get_particle_state(eob,epsys,p,&estate,0)){
						VECSUB(vec_to_part, state->co, estate.co);
						distance = VecLength(vec_to_part);

						//if(pd->forcefield==PFIELD_HARMONIC){
						//	//if(cfra < epa->time + radius){ /* radius is fade-in in ui */
						//	//	eforce*=(cfra-epa->time)/radius;
						//	//}
						//}
						//else{
						//	/* Limit minimum distance to effector particle so that */
						//	/* the force is not too big */
						//	if (distance < 0.001) distance = 0.001f;
						//}
						
						falloff=effector_falloff(pd,estate.vel,vec_to_part);

						if(falloff<=0.0f)
							;	/* don't do anything */
						else
							do_physical_effector(pd->forcefield,pd->f_strength,distance,
							falloff,epart->size,pd->f_damp,estate.vel,vec_to_part,
							state->vel,force_field,0);
					}
					else if(pd->forcefield==PFIELD_HARMONIC && cfra-framestep <= epa->dietime && cfra>epa->dietime){
						/* first step after key release */
						psys_get_particle_state(eob,epsys,p,&estate,1);
						VECADD(vel,vel,estate.vel);
						/* TODO: add rotation handling here too */
					}
				}

				if(epsys->lattice){
					end_latt_deform();
					epsys->lattice=0;
				}
			}
		}
	}
}

/************************************************/
/*			Newtonian physics					*/
/************************************************/
/* gathers all forces that effect particles and calculates a new state for the particle */
static void apply_particle_forces(int pa_no, ParticleData *pa, Object *ob, ParticleSystem *psys, ParticleSettings *part, float timestep, float dfra, float cfra, ParticleKey *state)
{
	ParticleKey states[5], tkey;
	float force[3],tvel[3],dx[4][3],dv[4][3];
	float dtime=dfra*timestep, time, pa_mass=part->mass, fac, fra=psys->cfra;
	int i, steps=1;
	
	/* maintain angular velocity */
	VECCOPY(state->ave,pa->state.ave);

	if(part->flag & PART_SIZEMASS)
		pa_mass*=pa->size;

	switch(part->integrator){
		case PART_INT_EULER:
			steps=1;
			break;
		case PART_INT_MIDPOINT:
			steps=2;
			break;
		case PART_INT_RK4:
			steps=4;
			break;
	}

	copy_particle_key(states,&pa->state,1);

	for(i=0; i<steps; i++){
		force[0]=force[1]=force[2]=0.0;
		tvel[0]=tvel[1]=tvel[2]=0.0;
		/* add effectors */
		do_effectors(pa_no,pa,states+i,ob,psys,force,tvel,dfra,fra);

		/* calculate air-particle interaction */
		if(part->dragfac!=0.0f){
			fac=-part->dragfac*pa->size*pa->size*VecLength(states[i].vel);
			VECADDFAC(force,force,states[i].vel,fac);
		}

		/* brownian force */
		if(part->brownfac!=0.0){
			force[0]+=(BLI_frand()-0.5f)*part->brownfac;
			force[1]+=(BLI_frand()-0.5f)*part->brownfac;
			force[2]+=(BLI_frand()-0.5f)*part->brownfac;
		}

		/* force to acceleration*/
		VecMulf(force,1.0f/pa_mass);

		/* add global acceleration (gravitation) */
		VECADD(force,force,part->acc);

		//VecMulf(force,dtime);
		
		/* calculate next state */
		VECADD(states[i].vel,states[i].vel,tvel);

		//VecMulf(force,0.5f*dt);
		switch(part->integrator){
			case PART_INT_EULER:
				VECADDFAC(state->co,states->co,states->vel,dtime);
				VECADDFAC(state->vel,states->vel,force,dtime);
				break;
			case PART_INT_MIDPOINT:
				if(i==0){
					VECADDFAC(states[1].co,states->co,states->vel,dtime*0.5f);
					VECADDFAC(states[1].vel,states->vel,force,dtime*0.5f);
					fra=psys->cfra+0.5f*dfra;
				}
				else{
					VECADDFAC(state->co,states->co,states[1].vel,dtime);
					VECADDFAC(state->vel,states->vel,force,dtime);
				}
				break;
			case PART_INT_RK4:
				switch(i){
					case 0:
						VECCOPY(dx[0],states->vel);
						VecMulf(dx[0],dtime);
						VECCOPY(dv[0],force);
						VecMulf(dv[0],dtime);

						VECADDFAC(states[1].co,states->co,dx[0],0.5f);
						VECADDFAC(states[1].vel,states->vel,dv[0],0.5f);
						fra=psys->cfra+0.5f*dfra;
						break;
					case 1:
						VECADDFAC(dx[1],states->vel,dv[0],0.5f);
						VecMulf(dx[1],dtime);
						VECCOPY(dv[1],force);
						VecMulf(dv[1],dtime);

						VECADDFAC(states[2].co,states->co,dx[1],0.5f);
						VECADDFAC(states[2].vel,states->vel,dv[1],0.5f);
						break;
					case 2:
						VECADDFAC(dx[2],states->vel,dv[1],0.5f);
						VecMulf(dx[2],dtime);
						VECCOPY(dv[2],force);
						VecMulf(dv[2],dtime);

						VECADD(states[3].co,states->co,dx[2]);
						VECADD(states[3].vel,states->vel,dv[2]);
						fra=cfra;
						break;
					case 3:
						VECADD(dx[3],states->vel,dv[2]);
						VecMulf(dx[3],dtime);
						VECCOPY(dv[3],force);
						VecMulf(dv[3],dtime);

						VECADDFAC(state->co,states->co,dx[0],1.0f/6.0f);
						VECADDFAC(state->co,state->co,dx[1],1.0f/3.0f);
						VECADDFAC(state->co,state->co,dx[2],1.0f/3.0f);
						VECADDFAC(state->co,state->co,dx[3],1.0f/6.0f);

						VECADDFAC(state->vel,states->vel,dv[0],1.0f/6.0f);
						VECADDFAC(state->vel,state->vel,dv[1],1.0f/3.0f);
						VECADDFAC(state->vel,state->vel,dv[2],1.0f/3.0f);
						VECADDFAC(state->vel,state->vel,dv[3],1.0f/6.0f);
				}
				break;
		}
		//VECADD(states[i+1].co,states[i+1].co,force);
	}

	/* damp affects final velocity */
	if(part->dampfac!=0.0)
		VecMulf(state->vel,1.0f-part->dampfac);

	/* finally we do guides */
	time=(cfra-pa->time)/pa->lifetime;
	CLAMP(time,0.0,1.0);

	VECCOPY(tkey.co,state->co);
	VECCOPY(tkey.vel,state->vel);
	tkey.time=state->time;
	if(do_guide(&tkey,pa_no,time,&psys->effectors)){
		VECCOPY(state->co,tkey.co);
		/* guides don't produce valid velocity */
		VECSUB(state->vel,tkey.co,pa->state.co);
		VecMulf(state->vel,1.0f/dtime);
		state->time=tkey.time;
	}
}
static void rotate_particle(ParticleSettings *part, ParticleData *pa, float dfra, float timestep, ParticleKey *state)
{
	float rotfac, rot1[4], rot2[4]={1.0,0.0,0.0,0.0}, dtime=dfra*timestep;

	if((part->flag & PART_ROT_DYN)==0){
		if(ELEM(part->avemode,PART_AVE_SPIN,PART_AVE_VEL)){
			float angle;
			float len1 = VecLength(pa->state.vel);
			float len2 = VecLength(state->vel);

			if(len1==0.0f || len2==0.0f)
				state->ave[0]=state->ave[1]=state->ave[2]=0.0f;
			else{
				Crossf(state->ave,pa->state.vel,state->vel);
				Normalize(state->ave);
				angle=Inpf(pa->state.vel,state->vel)/(len1*len2);
				VecMulf(state->ave,saacos(angle)/dtime);
			}
		}

		if(part->avemode == PART_AVE_SPIN)
			VecRotToQuat(state->vel,dtime*part->avefac,rot2);
	}

	rotfac=VecLength(state->ave);
	if(rotfac==0.0){ /* QuatOne (in VecRotToQuat) doesn't give unit quat [1,0,0,0]?? */
		rot1[0]=1.0;
		rot1[1]=rot1[2]=rot1[3]=0;
	}
	else{
		VecRotToQuat(state->ave,rotfac*dtime,rot1);
	}
	QuatMul(state->rot,rot1,pa->state.rot);
	QuatMul(state->rot,rot2,state->rot);

	/* keep rotation quat in good health */
	NormalQuat(state->rot);
}

/* convert from triangle barycentric weights to quad mean value weights */
static void intersect_dm_quad_weights(float *v1, float *v2, float *v3, float *v4, float *w)
{
	float co[3], vert[4][3];

	VECCOPY(vert[0], v1);
	VECCOPY(vert[1], v2);
	VECCOPY(vert[2], v3);
	VECCOPY(vert[3], v4);

	co[0]= v1[0]*w[0] + v2[0]*w[1] + v3[0]*w[2] + v4[0]*w[3];
	co[1]= v1[1]*w[0] + v2[1]*w[1] + v3[1]*w[2] + v4[1]*w[3];
	co[2]= v1[2]*w[0] + v2[2]*w[1] + v3[2]*w[2] + v4[2]*w[3];

	MeanValueWeights(vert, 4, co, w);
}

/* check intersection with a derivedmesh */
int psys_intersect_dm(Object *ob, DerivedMesh *dm, float *vert_cos, float *co1, float* co2, float *min_d, int *min_face, float *min_w,
						  float *face_minmax, float *pa_minmax, float radius, float *ipoint)
{
	MFace *mface=0;
	MVert *mvert=0;
	int i, totface, intersect=0;
	float cur_d, cur_uv[2], v1[3], v2[3], v3[3], v4[3], min[3], max[3], p_min[3],p_max[3];
	float cur_ipoint[3];
	
	if(dm==0){
		psys_disable_all(ob);

		dm=mesh_get_derived_final(ob,0);
		if(dm==0)
			mesh_get_derived_deform(ob,0);

		psys_enable_all(ob);

		if(dm==0)
			return 0;
	}

	

	if(pa_minmax==0){
		INIT_MINMAX(p_min,p_max);
		DO_MINMAX(co1,p_min,p_max);
		DO_MINMAX(co2,p_min,p_max);
	}
	else{
		VECCOPY(p_min,pa_minmax);
		VECCOPY(p_max,pa_minmax+3);
	}

	totface=dm->getNumFaces(dm);
	mface=dm->getFaceDataArray(dm,CD_MFACE);
	mvert=dm->getVertDataArray(dm,CD_MVERT);
	
	/* lets intersect the faces */
	for(i=0; i<totface; i++,mface++){
		if(vert_cos){
			VECCOPY(v1,vert_cos+3*mface->v1);
			VECCOPY(v2,vert_cos+3*mface->v2);
			VECCOPY(v3,vert_cos+3*mface->v3);
			if(mface->v4)
				VECCOPY(v4,vert_cos+3*mface->v4)
		}
		else{
			VECCOPY(v1,mvert[mface->v1].co);
			VECCOPY(v2,mvert[mface->v2].co);
			VECCOPY(v3,mvert[mface->v3].co);
			if(mface->v4)
				VECCOPY(v4,mvert[mface->v4].co)
		}

		if(face_minmax==0){
			INIT_MINMAX(min,max);
			DO_MINMAX(v1,min,max);
			DO_MINMAX(v2,min,max);
			DO_MINMAX(v3,min,max);
			if(mface->v4)
				DO_MINMAX(v4,min,max)
			if(AabbIntersectAabb(min,max,p_min,p_max)==0)
				continue;
		}
		else{
			VECCOPY(min, face_minmax+6*i);
			VECCOPY(max, face_minmax+6*i+3);
			if(AabbIntersectAabb(min,max,p_min,p_max)==0)
				continue;
		}

		if(radius>0.0f){
			if(SweepingSphereIntersectsTriangleUV(co1, co2, radius, v2, v3, v1, &cur_d, cur_ipoint)){
				if(cur_d<*min_d){
					*min_d=cur_d;
					VECCOPY(ipoint,cur_ipoint);
					*min_face=i;
					intersect=1;
				}
			}
			if(mface->v4){
				if(SweepingSphereIntersectsTriangleUV(co1, co2, radius, v4, v1, v3, &cur_d, cur_ipoint)){
					if(cur_d<*min_d){
						*min_d=cur_d;
						VECCOPY(ipoint,cur_ipoint);
						*min_face=i;
						intersect=1;
					}
				}
			}
		}
		else{
			if(LineIntersectsTriangle(co1, co2, v1, v2, v3, &cur_d, cur_uv)){
				if(cur_d<*min_d){
					*min_d=cur_d;
					min_w[0]= 1.0 - cur_uv[0] - cur_uv[1];
					min_w[1]= cur_uv[0];
					min_w[2]= cur_uv[1];
					min_w[3]= 0.0f;
					if(mface->v4)
						intersect_dm_quad_weights(v1, v2, v3, v4, min_w);
					*min_face=i;
					intersect=1;
				}
			}
			if(mface->v4){
				if(LineIntersectsTriangle(co1, co2, v1, v3, v4, &cur_d, cur_uv)){
					if(cur_d<*min_d){
						*min_d=cur_d;
						min_w[0]= 1.0 - cur_uv[0] - cur_uv[1];
						min_w[1]= 0.0f;
						min_w[2]= cur_uv[0];
						min_w[3]= cur_uv[1];
						intersect_dm_quad_weights(v1, v2, v3, v4, min_w);
						*min_face=i;
						intersect=1;
					}
				}
			}
		}
	}
	return intersect;
}
/* particle - mesh collision code */
/* in addition to basic point to surface collisions handles friction & damping,*/
/* angular momentum <-> linear momentum and swept sphere - mesh collisions */
/* 1. check for all possible deflectors for closest intersection on particle path */
/* 2. if deflection was found kill the particle or calculate new coordinates */
static void deflect_particle(Object *pob, ParticleSystemModifierData *psmd, ParticleSystem *psys, ParticleSettings *part, ParticleData *pa, int p, float dfra, float cfra, ParticleKey *state, int *pa_die){
	Object *ob, *min_ob;
	MFace *mface;
	MVert *mvert;
	DerivedMesh *dm;
	ListBase *lb=&psys->effectors;
	ParticleEffectorCache *ec;
	ParticleKey cstate;
	float imat[4][4];
	float co1[3],co2[3],def_loc[3],def_nor[3],unit_nor[3],def_tan[3],dvec[3],def_vel[3],dave[3],dvel[3];
	float pa_minmax[6];
	float min_w[4], zerovec[3]={0.0,0.0,0.0}, ipoint[3];
	float min_d,dotprod,damp,frict,o_len,d_len,radius=-1.0f;
	int min_face=0, intersect=1, through=0;
	short deflections=0, global=0;

	VECCOPY(def_loc,pa->state.co);
	VECCOPY(def_vel,pa->state.vel);

	/* 10 iterations to catch multiple deflections */
	if(lb->first) while(deflections<10){
		intersect=0;
		global=0;
		min_d=20000.0;
		min_ob=NULL;
		/* 1. */
		for(ec=lb->first; ec; ec=ec->next){
			if(ec->type & PSYS_EC_DEFLECT){
				ob= ec->ob;

				if(part->type!=PART_HAIR)
					where_is_object_time(ob,cfra);

				if(ob==pob){
					dm=psmd->dm;
					/* particles should not collide with emitter at birth */
					if(pa->time < cfra && pa->time >= psys->cfra)
						continue;
				}
				else
					dm=0;
				
				VECCOPY(co1,def_loc);
				VECCOPY(co2,state->co);

				if(ec->vert_cos==0){
					/* convert particle coordinates to object coordinates */
					Mat4Invert(imat,ob->obmat);

					Mat4MulVecfl(imat,co1);
					Mat4MulVecfl(imat,co2);
				}

				INIT_MINMAX(pa_minmax,pa_minmax+3);
				DO_MINMAX(co1,pa_minmax,pa_minmax+3);
				DO_MINMAX(co2,pa_minmax,pa_minmax+3);
				if(part->flag&PART_SIZE_DEFL){
					pa_minmax[0]-=pa->size;
					pa_minmax[1]-=pa->size;
					pa_minmax[2]-=pa->size;
					pa_minmax[3]+=pa->size;
					pa_minmax[4]+=pa->size;
					pa_minmax[5]+=pa->size;

					radius=pa->size;
				}

				if(ec->face_minmax==0 || AabbIntersectAabb(pa_minmax,pa_minmax+3,ec->ob_minmax,ec->ob_minmax+3))
					if(psys_intersect_dm(ob,dm,ec->vert_cos,co1,co2,&min_d,&min_face,min_w,
						ec->face_minmax,pa_minmax,radius,ipoint)){
						min_ob=ob;
						if(ec->vert_cos)
							global=1;
						else
							global=0;
					}
			}
		}

		/* 2. */
		if(min_ob){
			BLI_srandom((int)cfra+p);
			ob=min_ob;

			if(ob==pob){
				dm=psmd->dm;
			}
			else{
				psys_disable_all(ob);

				dm=mesh_get_derived_final(ob,0);

				psys_enable_all(ob);
			}

			mface=dm->getFaceDataArray(dm,CD_MFACE);
			mface+=min_face;
			mvert=dm->getVertDataArray(dm,CD_MVERT);


			/* permeability check */
			if(BLI_frand()<ob->pd->pdef_perm)
				through=1;
			else
				through=0;

			if(through==0 && (part->flag & PART_DIE_ON_COL || ob->pd->flag & PDEFLE_KILL_PART)){
				pa->dietime = cfra-(1.0f-min_d)*dfra;
				VecLerpf(def_loc,co1,co2,min_d);

				if(global==0)
					Mat4MulVecfl(ob->obmat,def_loc);

				VECCOPY(state->co,def_loc);
				VecLerpf(state->vel,pa->state.vel,state->vel,min_d);
				QuatInterpol(state->rot,pa->state.rot,state->rot,min_d);
				VecLerpf(state->ave,pa->state.ave,state->ave,min_d);

				*pa_die=1;

				/* particle is dead so we don't need to calculate further */
				deflections=10;

				/* store for reactors */
				copy_particle_key(&cstate,state,0);

				if(part->flag & PART_STICKY){
					pa->stick_ob=ob;
					pa->flag |= PARS_STICKY;
					//stick_particle_to_object(ob,pa,state);
				}
			}
			else{
				VecLerpf(def_loc,co1,co2,min_d);

				if(radius>0.0f){
					VECSUB(unit_nor,def_loc,ipoint);
				}
				else{
					/* get deflection point & normal */
					psys_interpolate_face(mvert,mface,0,0,min_w,ipoint,unit_nor,0,0,0,0);
					if(global){
						Mat4Mul3Vecfl(ob->obmat,unit_nor);
						Mat4MulVecfl(ob->obmat,ipoint);
					}
				}

				Normalize(unit_nor);

				VECSUB(dvec,co1,co2);
				/* scale to remaining length after deflection */
				VecMulf(dvec,1.0f-min_d);

				/* flip normal to face particle */
				if(Inpf(unit_nor,dvec)<0.0f)
					VecMulf(unit_nor,-1.0f);

				/* store for easy velocity calculation */
				o_len=VecLength(dvec);

				/* project particle movement to normal & create tangent */
				dotprod=Inpf(dvec,unit_nor);
				VECCOPY(def_nor,unit_nor);
				VecMulf(def_nor,dotprod);
				VECSUB(def_tan,def_nor,dvec);

				damp=ob->pd->pdef_damp+ob->pd->pdef_rdamp*2*(BLI_frand()-0.5f);

				/* create location after deflection */
				VECCOPY(dvec,def_nor);
				damp=ob->pd->pdef_damp+ob->pd->pdef_rdamp*2*(BLI_frand()-0.5f);
				CLAMP(damp,0.0,1.0);
				VecMulf(dvec,1.0f-damp);
				if(through)
					VecMulf(dvec,-1.0);
				
				frict=ob->pd->pdef_frict+ob->pd->pdef_rfrict*2.0f*(BLI_frand()-0.5f);
				CLAMP(frict,0.0,1.0);
				VECADDFAC(dvec,dvec,def_tan,1.0f-frict);

				/* store for easy velocity calculation */
				d_len=VecLength(dvec);

				/* just to be sure we don't hit the current face again */
				if(through){
					VECADDFAC(ipoint,ipoint,unit_nor,-0.0001f);
					VECADDFAC(def_loc,def_loc,unit_nor,-0.0001f);

					if(part->flag & PART_ROT_DYN){
						VECADDFAC(def_tan,def_tan,unit_nor,-0.0001f);
						VECADDFAC(def_nor,def_nor,unit_nor,-0.0001f);
					}
				}
				else{
					VECADDFAC(ipoint,ipoint,unit_nor,0.0001f);
					VECADDFAC(def_loc,def_loc,unit_nor,0.0001f);

					if(part->flag & PART_ROT_DYN){
						VECADDFAC(def_tan,def_tan,unit_nor,0.0001f);
						VECADDFAC(def_nor,def_nor,unit_nor,0.0001f);
					}
				}

				/* lets get back to global space */
				if(global==0){
					Mat4Mul3Vecfl(ob->obmat,dvec);
					Mat4MulVecfl(ob->obmat,ipoint);
					Mat4MulVecfl(ob->obmat,def_loc);/* def_loc remains as intersection point for next iteration */
				}

				/* store for reactors */
				VECCOPY(cstate.co,ipoint);
				VecLerpf(cstate.vel,pa->state.vel,state->vel,min_d);
				QuatInterpol(cstate.rot,pa->state.rot,state->rot,min_d);

				/* slightly unphysical but looks nice enough */
				if(part->flag & PART_ROT_DYN){
					if(global==0){
						Mat4Mul3Vecfl(ob->obmat,def_nor);
						Mat4Mul3Vecfl(ob->obmat,def_tan);
					}

					Normalize(def_tan);
					Normalize(def_nor);
					VECCOPY(unit_nor,def_nor);

					/* create normal velocity */
					VecMulf(def_nor,Inpf(pa->state.vel,def_nor));

					/* create tangential velocity */
					VecMulf(def_tan,Inpf(pa->state.vel,def_tan));
					
					/* angular velocity change due to tangential velocity */
					Crossf(dave,unit_nor,def_tan);
					VecMulf(dave,1.0f/pa->size);

					/* linear velocity change due to angular velocity */
					VecMulf(unit_nor,pa->size); /* point of impact from particle center */
					Crossf(dvel,pa->state.ave,unit_nor);

					if(through)
						VecMulf(def_nor,-1.0);

					VecMulf(def_nor,1.0f-damp);
					VECSUB(dvel,dvel,def_nor);

					VecMulf(dvel,1.0f-frict);
					VecMulf(dave,1.0f-frict);
				}
				
				if(d_len<0.001 && VecLength(pa->state.vel)<0.001){
					/* kill speed to stop slipping */
					VECCOPY(state->vel,zerovec);
					VECCOPY(state->co,def_loc);
					if(part->flag & PART_ROT_DYN)
						VECCOPY(state->ave,zerovec);
					deflections=10;
				}
				else{

					/* apply new coordinates */
					VECADD(state->co,def_loc,dvec);

					Normalize(dvec);

					/* we have to use original velocity because otherwise we get slipping	*/
					/* when forces like gravity balance out damping & friction				*/
					VecMulf(dvec,VecLength(pa->state.vel)*(d_len/o_len));
					VECCOPY(state->vel,dvec);

					if(part->flag & PART_ROT_DYN){
						VECADD(state->vel,state->vel,dvel);
						VecMulf(state->vel,0.5);
						VECADD(state->ave,state->ave,dave);
						VecMulf(state->ave,0.5);
					}
				}
			}
			deflections++;

			cstate.time=cfra-(1.0f-min_d)*dfra;
			//particle_react_to_collision(min_ob,pob,psys,pa,p,&cstate);
			push_reaction(pob,psys,p,PART_EVENT_COLLIDE,&cstate);
		}
		else
			return;
	}
}
/************************************************/
/*			Boid physics						*/
/************************************************/
static int boid_see_mesh(ListBase *lb, Object *pob, ParticleSystem *psys, float *vec1, float *vec2, float *loc, float *nor, float cfra)
{
	Object *ob, *min_ob;
	DerivedMesh *dm;
	MFace *mface;
	MVert *mvert;
	ParticleEffectorCache *ec;
	ParticleSystemModifierData *psmd=psys_get_modifier(pob,psys);
	float imat[4][4];
	float co1[3], co2[3], min_w[4], min_d;
	int min_face=0, intersect=0;

	if(lb->first){
		intersect=0;
		min_d=20000.0;
		min_ob=NULL;
		for(ec=lb->first; ec; ec=ec->next){
			if(ec->type & PSYS_EC_DEFLECT){
				ob= ec->ob;

				if(psys->part->type!=PART_HAIR)
					where_is_object_time(ob,cfra);

				if(ob==pob)
					dm=psmd->dm;
				else
					dm=0;

				VECCOPY(co1,vec1);
				VECCOPY(co2,vec2);

				if(ec->vert_cos==0){
					/* convert particle coordinates to object coordinates */
					Mat4Invert(imat,ob->obmat);

					Mat4MulVecfl(imat,co1);
					Mat4MulVecfl(imat,co2);
				}

				if(psys_intersect_dm(ob,dm,ec->vert_cos,co1,co2,&min_d,&min_face,min_w,ec->face_minmax,0,0,0))
					min_ob=ob;
			}
		}
		if(min_ob){
			ob=min_ob;

			if(ob==pob){
				dm=psmd->dm;
			}
			else{
				psys_disable_all(ob);

				dm=mesh_get_derived_deform(ob,0);

				psys_enable_all(ob);
			}

			mface=dm->getFaceDataArray(dm,CD_MFACE);
			mface+=min_face;
			mvert=dm->getVertDataArray(dm,CD_MVERT);

			/* get deflection point & normal */
			psys_interpolate_face(mvert,mface,0,0,min_w,loc,nor,0,0,0,0);

			VECADD(nor,nor,loc);
			Mat4MulVecfl(ob->obmat,loc);
			Mat4MulVecfl(ob->obmat,nor);
			VECSUB(nor,nor,loc);
			return 1;
		}
	}
	return 0;
}
/* vector calculus functions in 2d vs. 3d */
static void set_boid_vec_func(BoidVecFunc *bvf, int is_2d)
{
	if(is_2d){
		bvf->Addf = Vec2Addf;
		bvf->Subf = Vec2Subf;
		bvf->Mulf = Vec2Mulf;
		bvf->Length = Vec2Length;
		bvf->Normalize = Normalize2;
		bvf->Inpf = Inp2f;
		bvf->Copyf = Vec2Copyf;
	}
	else{
		bvf->Addf = VecAddf;
		bvf->Subf = VecSubf;
		bvf->Mulf = VecMulf;
		bvf->Length = VecLength;
		bvf->Normalize = Normalize;
		bvf->Inpf = Inpf;
		bvf->Copyf = VecCopyf;
	}
}
/* boids have limited processing capability so once there's too much information (acceleration) no more is processed */
static int add_boid_acc(BoidVecFunc *bvf, float lat_max, float tan_max, float *lat_accu, float *tan_accu, float *acc, float *dvec, float *vel)
{
	static float tangent[3];
	static float tan_length;

	if(vel){
		bvf->Copyf(tangent,vel);
		tan_length=bvf->Normalize(tangent);
		return 1;
	}
	else{
		float cur_tan, cur_lat;
		float tan_acc[3], lat_acc[3];
		int ret=0;

		bvf->Copyf(tan_acc,tangent);

		if(tan_length>0.0){
			bvf->Mulf(tan_acc,Inpf(tangent,dvec));

			bvf->Subf(lat_acc,dvec,tan_acc);
		}
		else{
			bvf->Copyf(tan_acc,dvec);
			lat_acc[0]=lat_acc[1]=lat_acc[2]=0.0f;
			*lat_accu=lat_max;
		}

		cur_tan=bvf->Length(tan_acc);
		cur_lat=bvf->Length(lat_acc);

		/* add tangential acceleration */
		if(*lat_accu+cur_lat<=lat_max){
			bvf->Addf(acc,acc,lat_acc);
			*lat_accu+=cur_lat;
			ret=1;
		}
		else{
			bvf->Mulf(lat_acc,(lat_max-*lat_accu)/cur_lat);
			bvf->Addf(acc,acc,lat_acc);
			*lat_accu=lat_max;
		}

		/* add lateral acceleration */
		if(*tan_accu+cur_tan<=tan_max){
			bvf->Addf(acc,acc,tan_acc);
			*tan_accu+=cur_tan;
			ret=1;
		}
		else{
			bvf->Mulf(tan_acc,(tan_max-*tan_accu)/cur_tan);
			bvf->Addf(acc,acc,tan_acc);
			*tan_accu=tan_max;
		}

		return ret;
	}
}
/* determines the acceleration that the boid tries to acchieve */
static void boid_brain(BoidVecFunc *bvf, ParticleData *pa, Object *ob, ParticleSystem *psys, ParticleSettings *part, KDTree *tree, float timestep, float cfra, float *acc, int *pa_die)
{
	ParticleData *pars=psys->particles;
	KDTreeNearest ptn[MAX_BOIDNEIGHBOURS+1];
	ParticleEffectorCache *ec=0;
	float dvec[3]={0.0,0.0,0.0}, ob_co[3], ob_nor[3];
	float avoid[3]={0.0,0.0,0.0}, velocity[3]={0.0,0.0,0.0}, center[3]={0.0,0.0,0.0};
	float cubedist[MAX_BOIDNEIGHBOURS+1];
	int i, n, neighbours=0, near, not_finished=1;

	float cur_vel;
	float lat_accu=0.0f, max_lat_acc=part->max_vel*part->max_lat_acc;
	float tan_accu=0.0f, max_tan_acc=part->max_vel*part->max_tan_acc;
	float avg_vel=part->average_vel*part->max_vel;

	acc[0]=acc[1]=acc[2]=0.0f;
	/* the +1 neighbour is because boid itself is in the tree */
	neighbours=BLI_kdtree_find_n_nearest(tree,part->boidneighbours+1,pa->state.co,NULL,ptn);

	for(n=1; n<neighbours; n++){
		cubedist[n]=(float)pow((double)(ptn[n].dist/pa->size),3.0);
		cubedist[n]=1.0f/MAX2(cubedist[n],1.0f);
	}

	/* initialize tangent */
	add_boid_acc(bvf,0.0,0.0,0,0,0,0,pa->state.vel);

	for(i=0; i<BOID_TOT_RULES && not_finished; i++){
		switch(part->boidrule[i]){
			case BOID_COLLIDE:
				/* collision avoidance */
				bvf->Copyf(dvec,pa->state.vel);
				bvf->Mulf(dvec,5.0f);
				bvf->Addf(dvec,dvec,pa->state.co);
				if(boid_see_mesh(&psys->effectors,ob,psys,pa->state.co,dvec,ob_co,ob_nor,cfra)){
					float probelen = bvf->Length(dvec);
					float proj;
					float oblen;

					Normalize(ob_nor);
					proj = bvf->Inpf(ob_nor,pa->state.vel);

					bvf->Subf(dvec,pa->state.co,ob_co);
					oblen=bvf->Length(dvec);

					bvf->Copyf(dvec,ob_nor);
					bvf->Mulf(dvec,-proj);
					bvf->Mulf(dvec,((probelen/oblen)-1.0f)*100.0f*part->boidfac[BOID_COLLIDE]);

					not_finished=add_boid_acc(bvf,max_lat_acc,max_tan_acc,&lat_accu,&tan_accu,acc,dvec,0);
				}
				break;
			case BOID_AVOID:
				/* predator avoidance */
				if(psys->effectors.first){
					for(ec=psys->effectors.first; ec; ec=ec->next){
						if(ec->type & PSYS_EC_EFFECTOR){
							Object *eob = ec->ob;
							PartDeflect *pd = eob->pd;

							if(pd->forcefield==PFIELD_FORCE && pd->f_strength<0.0){
								float distance;
								VECSUB(dvec,eob->obmat[3],pa->state.co);
								
								distance=Normalize(dvec);

								if(part->flag & PART_DIE_ON_COL && distance < pd->mindist){
									*pa_die=1;
									pa->dietime=cfra;
									i=BOID_TOT_RULES;
									break;
								}

								if(pd->flag&PFIELD_USEMAX && distance > pd->maxdist)
									;
								else{
									bvf->Mulf(dvec,part->boidfac[BOID_AVOID]*pd->f_strength/(float)pow((double)distance,(double)pd->f_power));

									not_finished=add_boid_acc(bvf,max_lat_acc,max_tan_acc,&lat_accu,&tan_accu,acc,dvec,0);
								}
							}
						}
						else if(ec->type & PSYS_EC_PARTICLE){
							Object *eob = ec->ob;
							ParticleSystem *epsys;
							ParticleSettings *epart;
							ParticleKey state;
							PartDeflect *pd;
							KDTreeNearest ptn2[MAX_BOIDNEIGHBOURS];
							int totepart, p, count;
							float distance;
							epsys= BLI_findlink(&eob->particlesystem,ec->psys_nbr);
							epart= epsys->part;
							pd= epart->pd;
							totepart= epsys->totpart;

							if(pd->forcefield==PFIELD_FORCE && pd->f_strength<0.0){
								count=BLI_kdtree_find_n_nearest(ec->tree,epart->boidneighbours,pa->state.co,NULL,ptn2);
								for(p=0; p<count; p++){
									state.time=-1.0;
									if(psys_get_particle_state(eob,epsys,ptn2[p].index,&state,0)){
										VECSUB(dvec, state.co, pa->state.co);

										distance = Normalize(dvec);

										if(part->flag & PART_DIE_ON_COL && distance < (epsys->particles+ptn2[p].index)->size){
											*pa_die=1;
											pa->dietime=cfra;
											i=BOID_TOT_RULES;
											break;
										}

										if(pd->flag&PFIELD_USEMAX && distance > pd->maxdist)
											;
										else{
											bvf->Mulf(dvec,part->boidfac[BOID_AVOID]*pd->f_strength/(float)pow((double)distance,(double)pd->f_power));

											not_finished=add_boid_acc(bvf,max_lat_acc,max_tan_acc,&lat_accu,&tan_accu,acc,dvec,0);
										}
									}
								}
							}
						}
					}
				}
				break;
			case BOID_CROWD:
				/* crowd avoidance */
				near=0;
				for(n=1; n<neighbours; n++){
					if(ptn[n].dist<2.0f*pa->size){
						bvf->Subf(dvec,pa->state.co,pars[ptn[n].index].state.co);
						bvf->Mulf(dvec,(2.0f*pa->size-ptn[n].dist)/ptn[n].dist);
						bvf->Addf(avoid,avoid,dvec);
						near++;
					}
					/* ptn[] is distance ordered so no need to check others */
					else break;
				}
				if(near){
					bvf->Mulf(avoid,part->boidfac[BOID_CROWD]*2.0f/timestep);

					not_finished=add_boid_acc(bvf,max_lat_acc,max_tan_acc,&lat_accu,&tan_accu,acc,avoid,0);
				}
				break;
			case BOID_CENTER:
				/* flock centering */
				if(neighbours>1){
					for(n=1; n<neighbours; n++){
						bvf->Addf(center,center,pars[ptn[n].index].state.co);
					}
					bvf->Mulf(center,1.0f/((float)neighbours-1.0f));

					bvf->Subf(dvec,center,pa->state.co);

					bvf->Mulf(dvec,part->boidfac[BOID_CENTER]*2.0f);

					not_finished=add_boid_acc(bvf,max_lat_acc,max_tan_acc,&lat_accu,&tan_accu,acc,dvec,0);
				}
				break;
			case BOID_AV_VEL:
				/* average velocity */
				cur_vel=bvf->Length(pa->state.vel);
				if(cur_vel>0.0){
					bvf->Copyf(dvec,pa->state.vel);
					bvf->Mulf(dvec,part->boidfac[BOID_AV_VEL]*(avg_vel-cur_vel)/cur_vel);
					not_finished=add_boid_acc(bvf,max_lat_acc,max_tan_acc,&lat_accu,&tan_accu,acc,dvec,0);
				}
				break;
			case BOID_VEL_MATCH:
				/* velocity matching */
				if(neighbours>1){
					for(n=1; n<neighbours; n++){
						bvf->Copyf(dvec,pars[ptn[n].index].state.vel);
						bvf->Mulf(dvec,cubedist[n]);
						bvf->Addf(velocity,velocity,dvec);
					}
					bvf->Mulf(velocity,1.0f/((float)neighbours-1.0f));

					bvf->Subf(dvec,velocity,pa->state.vel);

					bvf->Mulf(dvec,part->boidfac[BOID_VEL_MATCH]);

					not_finished=add_boid_acc(bvf,max_lat_acc,max_tan_acc,&lat_accu,&tan_accu,acc,dvec,0);
				}
				break;
			case BOID_GOAL:
				/* goal seeking */
				if(psys->effectors.first){
					for(ec=psys->effectors.first; ec; ec=ec->next){
						if(ec->type & PSYS_EC_EFFECTOR){
							Object *eob = ec->ob;
							PartDeflect *pd = eob->pd;
							float temp[4];

							if(pd->forcefield==PFIELD_FORCE && pd->f_strength>0.0){
								float distance;
								VECSUB(dvec,eob->obmat[3],pa->state.co);
								
								distance=Normalize(dvec);

								if(pd->flag&PFIELD_USEMAX && distance > pd->maxdist)
									;
								else{
									VecMulf(dvec,pd->f_strength*part->boidfac[BOID_GOAL]/(float)pow((double)distance,(double)pd->f_power));

									not_finished=add_boid_acc(bvf,max_lat_acc,max_tan_acc,&lat_accu,&tan_accu,acc,dvec,0);
								}
							}
							else if(pd->forcefield==PFIELD_GUIDE){
								float distance;

								where_on_path(eob, (cfra-pa->time)/pa->lifetime, temp, dvec);

								VECSUB(dvec,temp,pa->state.co);

								distance=Normalize(dvec);

								if(pd->flag&PFIELD_USEMAX && distance > pd->maxdist)
									;
								else{
									VecMulf(dvec,pd->f_strength*part->boidfac[BOID_GOAL]/(float)pow((double)distance,(double)pd->f_power));

									not_finished=add_boid_acc(bvf,max_lat_acc,max_tan_acc,&lat_accu,&tan_accu,acc,dvec,0);
								}
							}
						}
						else if(ec->type & PSYS_EC_PARTICLE){
							Object *eob = ec->ob;
							ParticleSystem *epsys;
							ParticleSettings *epart;
							ParticleKey state;
							PartDeflect *pd;
							KDTreeNearest ptn2[MAX_BOIDNEIGHBOURS];
							int totepart, p, count;
							float distance;
							epsys= BLI_findlink(&eob->particlesystem,ec->psys_nbr);
							epart= epsys->part;
							pd= epart->pd;
							totepart= epsys->totpart;

							if(pd->forcefield==PFIELD_FORCE && pd->f_strength>0.0){
								count=BLI_kdtree_find_n_nearest(ec->tree,epart->boidneighbours,pa->state.co,NULL,ptn2);
								for(p=0; p<count; p++){
									state.time=-1.0;
									if(psys_get_particle_state(eob,epsys,ptn2[p].index,&state,0)){
										VECSUB(dvec, state.co, pa->state.co);

										distance = Normalize(dvec);
										
										if(pd->flag&PFIELD_USEMAX && distance > pd->maxdist)
											;
										else{
											bvf->Mulf(dvec,part->boidfac[BOID_AVOID]*pd->f_strength/(float)pow((double)distance,(double)pd->f_power));

											not_finished=add_boid_acc(bvf,max_lat_acc,max_tan_acc,&lat_accu,&tan_accu,acc,dvec,0);
										}
									}
								}
							}
						}
					}
				}
				break;
			case BOID_LEVEL:
				/* level flight */
				if((part->flag & PART_BOIDS_2D)==0){
					dvec[0]=dvec[1]=0.0;
					dvec[2]=-pa->state.vel[2];

					VecMulf(dvec,part->boidfac[BOID_LEVEL]);
					not_finished=add_boid_acc(bvf,max_lat_acc,max_tan_acc,&lat_accu,&tan_accu,acc,dvec,0);
				}
				break;
		}
	}
}
/* tries to realize the wanted acceleration */
static void boid_body(BoidVecFunc *bvf, ParticleData *pa, ParticleSystem *psys, ParticleSettings *part, float timestep, float *acc, ParticleKey *state)
{
	float dvec[3], bvec[3], length, max_vel=part->max_vel;
	float *q2, q[4];
	float g=9.81f, pa_mass=part->mass;
	float yvec[3]={0.0,1.0,0.0}, zvec[3]={0.0,0.0,-1.0}, bank;

	/* apply new velocity, location & rotation */
	copy_particle_key(state,&pa->state,0);

	if(part->flag & PART_SIZEMASS)
		pa_mass*=pa->size;

	/* by regarding the acceleration as a force at this stage we*/
	/* can get better controll allthough it's a bit unphysical	*/
	bvf->Mulf(acc,1.0f/pa_mass);

	bvf->Copyf(dvec,acc);
	bvf->Mulf(dvec,timestep*timestep*0.5f);

	bvf->Copyf(bvec,state->vel);
	bvf->Mulf(bvec,timestep);
	bvf->Addf(dvec,dvec,bvec);
	bvf->Addf(state->co,state->co,dvec);
	
	/* air speed from wind effectors */
	if(psys->effectors.first){
		ParticleEffectorCache *ec;
		for(ec=psys->effectors.first; ec; ec=ec->next){
			if(ec->type & PSYS_EC_EFFECTOR){
				Object *eob = ec->ob;
				PartDeflect *pd = eob->pd;

				if(pd->forcefield==PFIELD_WIND && pd->f_strength!=0.0){
					float distance, wind[3];
					VecCopyf(wind,eob->obmat[2]);
					distance=VecLenf(state->co,eob->obmat[3]);

					if (distance < 0.001) distance = 0.001f;

					if(pd->flag&PFIELD_USEMAX && distance > pd->maxdist)
						;
					else{
						Normalize(wind);
						VecMulf(wind,pd->f_strength/(float)pow((double)distance,(double)pd->f_power));
						bvf->Addf(state->co,state->co,wind);
					}
				}
			}
		}
	}


	if((part->flag & PART_BOIDS_2D)==0 && pa->state.vel[0]!=0.0 && pa->state.vel[0]!=0.0 && pa->state.vel[0]!=0.0){
		Crossf(yvec,state->vel,zvec);

		Normalize(yvec);

		bank=Inpf(yvec,acc);

		bank=-(float)atan((double)(bank/g));

		bank*=part->banking;

		bank-=pa->bank;
		if(bank>M_PI*part->max_bank){
			bank=pa->bank+(float)M_PI*part->max_bank;
		}
		else if(bank<-M_PI*part->max_bank){
			bank=pa->bank-(float)M_PI*part->max_bank;
		}
		else
			bank+=pa->bank;

		pa->bank=bank;
	}
	else{
		bank=0.0;
	}


	VecRotToQuat(state->vel,bank,q);

	VECCOPY(dvec,state->vel);
	VecMulf(dvec,-1.0f);
	q2= vectoquat(dvec, OB_POSX, OB_POSZ);

	QuatMul(state->rot,q,q2);

	bvf->Mulf(acc,timestep);
	bvf->Addf(state->vel,state->vel,acc);

	if(part->flag & PART_BOIDS_2D){
		state->vel[2]=0.0;
		state->co[2]=part->groundz;

		if(psys->keyed_ob){
			Object *zob=psys->keyed_ob;
			int min_face;
			float co1[3],co2[3],min_d=2.0,min_w[4],imat[4][4];
			VECCOPY(co1,state->co);
			VECCOPY(co2,state->co);

			co1[2]=1000.0f;
			co2[2]=-1000.0f;

			Mat4Invert(imat,zob->obmat);
			Mat4MulVecfl(imat,co1);
			Mat4MulVecfl(imat,co2);

			if(psys_intersect_dm(zob,0,0,co1,co2,&min_d,&min_face,min_w,0,0,0,0)){
				DerivedMesh *dm;
				MFace *mface;
				MVert *mvert;
				float loc[3],nor[3],q1[4];

				psys_disable_all(zob);
				dm=mesh_get_derived_final(zob,0);
				psys_enable_all(zob);

				mface=dm->getFaceDataArray(dm,CD_MFACE);
				mface+=min_face;
				mvert=dm->getVertDataArray(dm,CD_MVERT);

				/* get deflection point & normal */
				psys_interpolate_face(mvert,mface,0,0,min_w,loc,nor,0,0,0,0);

				Mat4MulVecfl(zob->obmat,loc);
				Mat4Mul3Vecfl(zob->obmat,nor);

				Normalize(nor);

				VECCOPY(state->co,loc);

				zvec[2]=1.0;

				Crossf(loc,zvec,nor);

				bank=VecLength(loc);
				if(bank>0.0){
					bank=saasin(bank);

					VecRotToQuat(loc,bank,q);

					QUATCOPY(q1,state->rot);

					QuatMul(state->rot,q,q1);
				}
			}
		}
	}

	length=bvf->Length(state->vel);
	if(length > max_vel)
		bvf->Mulf(state->vel,max_vel/length);
}
/************************************************/
/*			Hair								*/
/************************************************/
void save_hair(Object *ob, ParticleSystem *psys, ParticleSystemModifierData *psmd, float cfra){
	ParticleData *pa;
	HairKey *key;
	int totpart;
	int i;

	Mat4Invert(ob->imat,ob->obmat);
	
	psys->lattice=psys_get_lattice(ob,psys);

	if(psys->totpart==0) return;

	totpart=psys->totpart;
	
	/* save new keys for elements if needed */
	for(i=0,pa=psys->particles; i<totpart; i++,pa++) {
		/* first time alloc */
		if(pa->totkey==0 || pa->hair==NULL) {
			pa->hair = MEM_callocN((psys->part->hair_step + 1) * sizeof(HairKey), "HairKeys");
			pa->totkey = 0;
		}

		key = pa->hair + pa->totkey;

		/* convert from global to geometry space */
		VecCopyf(key->co, pa->state.co);
		Mat4MulVecfl(ob->imat, key->co);

		if(pa->totkey) {
			VECSUB(key->co, key->co, pa->hair->co);
			psys_vec_rot_to_face(psmd->dm, pa, key->co);
		}

		key->time = pa->state.time;

		key->weight = 1.0f - key->time / 100.0f;

		pa->totkey++;

		/* root is always in the origin of hair space so we set it to be so after the last key is saved*/
		if(pa->totkey == psys->part->hair_step + 1)
			pa->hair->co[0] = pa->hair->co[1] = pa->hair->co[2] = 0.0f;
	}
}
/************************************************/
/*			System Core							*/
/************************************************/
/* unbaked particles are calculated dynamically */
static void dynamics_step(Object *ob, ParticleSystem *psys, ParticleSystemModifierData *psmd, float cfra,
						  float *vg_vel, float *vg_tan, float *vg_rot, float *vg_size)
{
	ParticleData *pa;
	ParticleKey *outstate, *key;
	ParticleSettings *part=psys->part;
	KDTree *tree=0;
	BoidVecFunc bvf;
	IpoCurve *icu_esize=find_ipocurve(part->ipo,PART_EMIT_SIZE);
	Material *ma=give_current_material(ob,part->omat);
	float timestep;
	int p, totpart, pa_die;
	/* current time */
	float ctime, ipotime;
	/* frame & time changes */
	float dfra, dtime, pa_dtime, pa_dfra=0.0;
	float birthtime, dietime;
	
	/* where have we gone in time since last time */
	dfra= cfra - psys->cfra;

	totpart=psys->totpart;

	timestep=psys_get_timestep(part);
	dtime= dfra*timestep;
	ctime= cfra*timestep;
	ipotime= cfra;

	if(part->flag&PART_ABS_TIME && part->ipo){
		calc_ipo(part->ipo, cfra);
		execute_ipo((ID *)part, part->ipo);
	}

	if(dfra<0.0){
		float *vg_size=0;
		if(part->type==PART_REACTOR)
			vg_size=psys_cache_vgroup(psmd->dm,psys,PSYS_VG_SIZE);

		for(p=0, pa=psys->particles; p<totpart; p++,pa++){
			if(pa->flag & (PARS_NO_DISP+PARS_UNEXIST)) continue;

			/* set correct ipo timing */
			if((part->flag&PART_ABS_TIME)==0 && part->ipo){
				ipotime=100.0f*(cfra-pa->time)/pa->lifetime;
				calc_ipo(part->ipo, ipotime);
				execute_ipo((ID *)part, part->ipo);
			}
			pa->size=psys_get_size(ob,ma,psmd,icu_esize,psys,part,pa,vg_size);

			if(part->type==PART_REACTOR)
				initialize_particle(pa,p,ob,psys,psmd);

			reset_particle(pa,psys,psmd,ob,dtime,cfra,vg_vel,vg_tan,vg_rot);

			if(cfra>pa->time && part->flag & PART_LOOP && (part->flag & PART_LOOP_INSTANT)==0){
				pa->loop=(short)((cfra-pa->time)/pa->lifetime)+1;
				pa->alive=PARS_UNBORN;
			}
			else{
				pa->loop=0;
				if(cfra<=pa->time)
					pa->alive=PARS_UNBORN;
						/* without dynamics the state is allways known so no need to kill */
				else if(ELEM(part->phystype,PART_PHYS_NO,PART_PHYS_KEYED)==0)
					pa->alive=PARS_KILLED;
			}
		}

		if(vg_size)
			MEM_freeN(vg_size);

		//if(part->phystype==PART_PHYS_SOLID)
		//	reset_to_first_fragment(psys);
	}
	else{
		BLI_srandom(31415926 + (int)cfra + psys->seed);

		/* outstate is used so that particles are updated in parallel */
		outstate=MEM_callocN(totpart*sizeof(ParticleKey),"Particle Outstates");
		
		/* update effectors */
		if(psys->effectors.first)
			psys_end_effectors(psys);

		psys_init_effectors(ob,part->eff_group,psys);
		
		if(psys->effectors.first)
			precalc_effectors(ob,psys,psmd);

		if(part->phystype==PART_PHYS_BOIDS){
			/* create particle tree for fast inter-particle comparisons */
			tree=BLI_kdtree_new(totpart);
			for(p=0, pa=psys->particles; p<totpart; p++,pa++){
				if(pa->flag & (PARS_NO_DISP+PARS_UNEXIST) || pa->alive!=PARS_ALIVE)
					continue;

				BLI_kdtree_insert(tree, p, pa->state.co, NULL);
			}
			BLI_kdtree_balance(tree);
			set_boid_vec_func(&bvf,part->flag&PART_BOIDS_2D);
		}

		/* main loop: calculate physics for all particles */
		for(p=0, pa=psys->particles, key=outstate; p<totpart; p++,pa++,key++){
			if(pa->flag & (PARS_NO_DISP|PARS_UNEXIST)) continue;

			copy_particle_key(key,&pa->state,1);
			
			/* set correct ipo timing */
			if((part->flag&PART_ABS_TIME)==0 && part->ipo){
				ipotime=100.0f*(cfra-pa->time)/pa->lifetime;
				calc_ipo(part->ipo, ipotime);
				execute_ipo((ID *)part, part->ipo);
			}
			pa->size=psys_get_size(ob,ma,psmd,icu_esize,psys,part,pa,vg_size);

			pa_die=0;

			if(pa->alive==PARS_UNBORN || pa->alive==PARS_KILLED || ELEM(part->phystype,PART_PHYS_NO,PART_PHYS_KEYED)){
				/* allways reset particles to emitter before birth */
				reset_particle(pa,psys,psmd,ob,dtime,cfra,vg_vel,vg_tan,vg_rot);
				copy_particle_key(key,&pa->state,1);
			}

			if(dfra>0.0 || psys->recalc){
				
				if(psys->reactevents.first && ELEM(pa->alive,PARS_DEAD,PARS_KILLED)==0)
					react_to_events(psys,p);

				pa_dfra= dfra;
				pa_dtime= dtime;

				if(pa->flag & PART_LOOP && pa->flag & PART_LOOP_INSTANT)
					birthtime=pa->dietime;
				else
					birthtime=pa->time+pa->loop*pa->lifetime;

				dietime=birthtime+pa->lifetime;

				if(birthtime < cfra && birthtime >= psys->cfra){
					/* particle is born some time between this and last step*/
					pa->alive=PARS_ALIVE;
					pa_dfra= cfra - birthtime;
					pa_dtime= pa_dfra*timestep;
				}
				else if(dietime <= cfra && psys->cfra < dietime){
					/* particle dies some time between this and last step */
					pa_dfra= dietime - psys->cfra;
					pa_dtime= pa_dfra*timestep;
					pa_die=1;
				}
				else if(dietime < cfra){
					/* TODO: figure out if there's something to be done when particle is dead */
				}

				copy_particle_key(key,&pa->state,1);

				if(dfra>0.0 && pa->alive==PARS_ALIVE){
					switch(part->phystype){
						case PART_PHYS_NEWTON:
							/* do global forces & effectors */
							apply_particle_forces(p,pa,ob,psys,part,timestep,pa_dfra,cfra,key);

							/* deflection */
							deflect_particle(ob,psmd,psys,part,pa,p,pa_dfra,cfra,key,&pa_die);

							/* rotations */
							rotate_particle(part,pa,pa_dfra,timestep,key);

							break;
						case PART_PHYS_BOIDS:
						{
							float acc[3];
							boid_brain(&bvf,pa,ob,psys,part,tree,timestep,cfra,acc,&pa_die);
							if(pa_die==0)
								boid_body(&bvf,pa,psys,part,timestep,acc,key);
							break;
						}
					}

					push_reaction(ob,psys,p,PART_EVENT_NEAR,key);

					if(pa_die){
						push_reaction(ob,psys,p,PART_EVENT_DEATH,key);

						if(part->flag & PART_LOOP){
							pa->loop++;

							if(part->flag & PART_LOOP_INSTANT){
								reset_particle(pa,psys,psmd,ob,0.0,cfra,vg_vel,vg_tan,vg_rot);
								pa->alive=PARS_ALIVE;
								copy_particle_key(key,&pa->state,1);
							}
							else
								pa->alive=PARS_UNBORN;
						}
						else{
							pa->alive=PARS_DEAD;
							key->time=pa->dietime;

							if(pa->flag&PARS_STICKY)
								psys_key_to_object(pa->stick_ob,key,0);
						}
					}
					else
						key->time=cfra;
				}
			}
		}
		/* apply outstates to particles */
		for(p=0, pa=psys->particles, key=outstate; p<totpart; p++,pa++,key++)
			copy_particle_key(&pa->state,key,1);

		MEM_freeN(outstate);
	}
	if(psys->reactevents.first)
		BLI_freelistN(&psys->reactevents);

	if(tree)
		BLI_kdtree_free(tree);
}

/* check if path cache or children need updating and do it if needed */
static void psys_update_path_cache(Object *ob, ParticleSystemModifierData *psmd, ParticleSystem *psys, float cfra)
{
	ParticleSettings *part=psys->part;
	ParticleEditSettings *pset=&G.scene->toolsettings->particle;
	int distr=0,alloc=0;
	int child_nbr= (psys->renderdata)? part->ren_child_nbr: part->child_nbr;

	if((psys->part->childtype && psys->totchild != psys->totpart*child_nbr) || psys->recalc&PSYS_ALLOC)
		alloc=1;

	if(alloc || psys->recalc&PSYS_DISTR || (psys->vgroup[PSYS_VG_DENSITY] && (G.f & G_WEIGHTPAINT)))
		distr=1;

	if(distr){
		if(alloc)
			alloc_particles(ob,psys,psys->totpart);

		if(get_alloc_child_particles_tot(psys)) {
			/* don't generate children while computing the hair keys */
			if(!(psys->part->type == PART_HAIR) || (psys->flag & PSYS_HAIR_DONE)) {
				distribute_particles(ob,psys,PART_FROM_CHILD);

				if(part->from!=PART_FROM_PARTICLE && part->childtype==PART_CHILD_FACES && part->parents!=0.0)
					psys_find_parents(ob,psmd,psys);
			}
		}
	}

	if((part->type==PART_HAIR || psys->flag&PSYS_KEYED) && (psys_in_edit_mode(psys)
		|| part->draw_as==PART_DRAW_PATH || part->draw&PART_DRAW_KEYS)){
		psys_cache_paths(ob, psys, cfra, 0);

		/* for render, child particle paths are computed on the fly */
		if(part->childtype) {
			if(((psys->totchild!=0)) || (psys_in_edit_mode(psys) && (pset->flag&PE_SHOW_CHILD)))
				psys_cache_child_paths(ob, psys, cfra, 0);
		}
	}
	else if(psys->pathcache)
		psys_free_path_cache(psys);
}

static void hair_step(Object *ob, ParticleSystemModifierData *psmd, ParticleSystem *psys, float cfra)
{
	ParticleSettings *part = psys->part;

	if(psys->recalc & PSYS_DISTR)
		/* need this for changing subsurf levels */
		psys_calc_dmfaces(ob, psmd->dm, psys);

	if(psys->effectors.first)
		psys_end_effectors(psys);

	psys_init_effectors(ob,part->eff_group,psys);
	if(psys->effectors.first)
		precalc_effectors(ob,psys,psmd);
		
	if(psys_in_edit_mode(psys))
		PE_recalc_world_cos(ob, psys);

	psys_update_path_cache(ob,psmd,psys,cfra);
}

/* updates cached particles' alive & other flags etc..*/
static void cached_step(Object *ob, ParticleSystemModifierData *psmd, ParticleSystem *psys, float cfra, float *vg_size)
{
	ParticleSettings *part=psys->part;
	ParticleData *pa;
	ParticleKey state;
	IpoCurve *icu_esize=find_ipocurve(part->ipo,PART_EMIT_SIZE);
	Material *ma=give_current_material(ob,part->omat);
	int p;
	float ipotime=cfra, disp;

	/* deprecated */
	//if(psys->recalc&PSYS_DISTR){
	//	/* The dm could have been changed so particle emitter element	 */
	//	/* indices might be wrong. There's really no "nice" way to handle*/ 
	//	/* this so we just try not to crash by correcting indices.		 */
	//	int totnum=-1;
	//	switch(part->from){
	//		case PART_FROM_VERT:
	//			totnum=psmd->dm->getNumVerts(psmd->dm);
	//			break;
	//		case PART_FROM_FACE:
	//		case PART_FROM_VOLUME:
	//			totnum=psmd->dm->getNumFaces(psmd->dm);
	//			break;
	//	}

	//	if(totnum==0){
	//		/* Now we're in real trouble, there's no emitter elements!! */
	//		for(p=0, pa=psys->particles; p<psys->totpart; p++,pa++)
	//			pa->num=-1;
	//	}
	//	else if(totnum>0){
	//		for(p=0, pa=psys->particles; p<psys->totpart; p++,pa++)
	//			pa->num=pa->num%totnum;
	//	}
	//}

	if(psys->effectors.first)
		psys_end_effectors(psys);
	
	//if(part->flag & (PART_BAKED_GUIDES+PART_BAKED_DEATHS)){
		psys_init_effectors(ob,part->eff_group,psys);
		if(psys->effectors.first)
			precalc_effectors(ob,psys,psmd);
	//}
	
	disp= (float)get_current_display_percentage(psys)/50.0f-1.0f;

	for(p=0, pa=psys->particles; p<psys->totpart; p++,pa++){
		if((part->flag&PART_ABS_TIME)==0 && part->ipo){
			ipotime=100.0f*(cfra-pa->time)/pa->lifetime;
			calc_ipo(part->ipo, ipotime);
			execute_ipo((ID *)part, part->ipo);
		}
		pa->size= psys_get_size(ob,ma,psmd,icu_esize,psys,part,pa,vg_size);

		psys->lattice=psys_get_lattice(ob,psys);

		/* update alive status and push events */
		if(pa->time>cfra)
			pa->alive=PARS_UNBORN;
		else if(pa->dietime<=cfra){
			if(pa->dietime>psys->cfra){
				state.time=pa->dietime;
				psys_get_particle_state(ob,psys,p,&state,1);
				push_reaction(ob,psys,p,PART_EVENT_DEATH,&state);
			}
			pa->alive=PARS_DEAD;
		}
		else{
			pa->alive=PARS_ALIVE;
			state.time=cfra;
			psys_get_particle_state(ob,psys,p,&state,1);
			state.time=cfra;
			push_reaction(ob,psys,p,PART_EVENT_NEAR,&state);
		}

		if(psys->lattice){
			end_latt_deform();
			psys->lattice=0;
		}

		if(pa->r_rot[0] > disp)
			pa->flag |= PARS_NO_DISP;
		else
			pa->flag &= ~PARS_NO_DISP;
	}
}
/* Calculates the next state for all particles of the system */
/* In particles code most fra-ending are frames, time-ending are fra*timestep (seconds)*/
static void system_step(Object *ob, ParticleSystem *psys, ParticleSystemModifierData *psmd, float cfra)
{
	ParticleSettings *part;
	ParticleData *pa;
	int totpart,oldtotpart=0,p;
	float disp, *vg_vel=0, *vg_tan=0, *vg_rot=0, *vg_size=0;
	int init=0,distr=0,alloc=0;
	int child_nbr;

	/*----start validity checks----*/

	part=psys->part;

	if(part->flag&PART_ABS_TIME && part->ipo){
		calc_ipo(part->ipo, cfra);
		execute_ipo((ID *)part, part->ipo);
	}

	if(part->from!=PART_FROM_PARTICLE)
		vg_size=psys_cache_vgroup(psmd->dm,psys,PSYS_VG_SIZE);

	if(part->type == PART_HAIR) {
		if(psys->flag & PSYS_HAIR_DONE) {
			hair_step(ob, psmd, psys, cfra);
			psys->cfra = cfra;
			psys->recalc = 0;
			return;
		}
	}
	else if(part->phystype != PART_PHYS_NO) {	/* cache shouldn't be used for none physics */
		if(psys->recalc && (psys->flag & PSYS_PROTECT_CACHE) == 0)
			clear_particles_from_cache(ob,psys,(int)cfra);
		else if(get_particles_from_cache(ob, psys, (int)cfra)) {
			cached_step(ob,psmd,psys,cfra,vg_size);
			psys->cfra=cfra;
			psys->recalc = 0;
			return;
		}
	}

	/* if still here react to events */

	if(psys->recalc&PSYS_TYPE) {
		/* system type has changed so set sensible defaults and clear non applicable flags */
		if(part->from == PART_FROM_PARTICLE) {
			if(part->type != PART_REACTOR)
				part->from = PART_FROM_FACE;
			if(part->distr == PART_DISTR_GRID)
				part->distr = PART_DISTR_JIT;
		}

		if(psys->part->phystype != PART_PHYS_KEYED)
			psys->flag &= ~PSYS_KEYED;

		if(part->type == PART_HAIR) {
			part->draw_as = PART_DRAW_PATH;
			part->rotfrom = PART_ROT_IINCR;
		}
		else
			free_hair(psys);

		psys->recalc &= ~PSYS_TYPE;
		alloc = 1;

		/* this is a bad level call, but currently type change
		 * can happen after redraw, so force redraw from here */
		allqueue(REDRAWBUTSOBJECT, 0);
	}
	else
		oldtotpart = psys->totpart;

	if(part->distr == PART_DISTR_GRID)
		totpart = part->grid_res * part->grid_res * part->grid_res;
	else
		totpart = psys->part->totpart;

	child_nbr= (psys->renderdata)? part->ren_child_nbr: part->child_nbr;
	if(oldtotpart != totpart || psys->recalc&PSYS_ALLOC || (psys->part->childtype && psys->totchild != psys->totpart*child_nbr))
		alloc = 1;

	if(alloc || psys->recalc&PSYS_DISTR || (psys->vgroup[PSYS_VG_DENSITY] && (G.f & G_WEIGHTPAINT) && ob==OBACT))
		distr = 1;

	if(distr || psys->recalc&PSYS_INIT)
		init = 1;

	if(init) {
		if(distr) {
			if(alloc)
				alloc_particles(ob, psys, totpart);

			distribute_particles(ob, psys, part->from);

			if(get_alloc_child_particles_tot(psys))
				distribute_particles(ob, psys, PART_FROM_CHILD);
		}
		initialize_all_particles(ob, psys, psmd);

		if(alloc)
			reset_all_particles(ob, psys, psmd, 0.0, cfra, oldtotpart);

		/* flag for possible explode modifiers after this system */
		psmd->flag |= eParticleSystemFlag_Pars;
	}


	if(part->phystype==PART_PHYS_KEYED && psys->flag&PSYS_FIRST_KEYED)
		psys_count_keyed_targets(ob,psys);

	if(part->from!=PART_FROM_PARTICLE){
		vg_vel=psys_cache_vgroup(psmd->dm,psys,PSYS_VG_VEL);
		vg_tan=psys_cache_vgroup(psmd->dm,psys,PSYS_VG_TAN);
		vg_rot=psys_cache_vgroup(psmd->dm,psys,PSYS_VG_ROT);
	}

	/* set particles to be not calculated */
	disp= (float)get_current_display_percentage(psys)/50.0f-1.0f;

	for(p=0, pa=psys->particles; p<totpart; p++,pa++){
		if(pa->r_rot[0] > disp)
			pa->flag |= PARS_NO_DISP;
		else
			pa->flag &= ~PARS_NO_DISP;
	}

	/* ok now we're all set so let's go */
	if(psys->totpart)
		dynamics_step(ob,psys,psmd,cfra,vg_vel,vg_tan,vg_rot,vg_size);

	psys->recalc = 0;
	psys->cfra=cfra;

	if(part->type!=PART_HAIR)
		write_particles_to_cache(ob, psys, cfra);

	/* for keyed particles the path is allways known so it can be drawn */
	if(part->phystype==PART_PHYS_KEYED && psys->flag&PSYS_FIRST_KEYED){
		set_keyed_keys(ob, psys);
		psys_update_path_cache(ob,psmd,psys,(int)cfra);
	}
	else if(psys->pathcache)
		psys_free_path_cache(psys);

	if(vg_vel)
		MEM_freeN(vg_vel);

	if(psys->lattice){
		end_latt_deform();
		psys->lattice=0;
	}
}

void psys_to_softbody(Object *ob, ParticleSystem *psys, int force_recalc)
{
	SoftBody *sb;
	short softflag; 

	if((psys->softflag&OB_SB_ENABLE)==0) return;

	if(psys->recalc || force_recalc)
		psys->softflag|=OB_SB_REDO;

	/* let's replace the object's own softbody with the particle softbody */
	/* a temporary solution before cloth simulation is implemented, jahka */

	/* save these */
	sb=ob->soft;
	softflag=ob->softflag;

	/* swich to new ones */
	ob->soft=psys->soft;
	ob->softflag=psys->softflag;

	/* do softbody */
	sbObjectStep(ob, (float)G.scene->r.cfra, NULL, psys_count_keys(psys));

	/* return things back to normal */
	psys->soft=ob->soft;
	psys->softflag=ob->softflag;
	
	ob->soft=sb;
	ob->softflag=softflag;
}
static int hair_needs_recalc(ParticleSystem *psys)
{
	if((psys->flag & PSYS_EDITED)==0 && (
		(psys->flag & PSYS_HAIR_DONE)==0
		|| psys->recalc & PSYS_RECALC_HAIR)
		) {
		psys->recalc &= ~PSYS_RECALC_HAIR;
		return 1;
	}

	return 0;
}
/* main particle update call, checks that things are ok on the large scale before actual particle calculations */
void particle_system_update(Object *ob, ParticleSystem *psys){

	ParticleSystemModifierData *psmd=0;
	float cfra;

	if(!psys_check_enabled(ob, psys))
		return;

	cfra=bsystem_time(ob,(float)CFRA,0.0);
	psmd= psys_get_modifier(ob, psys);

	/* system was already updated from modifier stack */
	if(psmd->flag&eParticleSystemFlag_psys_updated) {
		psmd->flag &= ~eParticleSystemFlag_psys_updated;
		/* make sure it really was updated to cfra */
		if(psys->cfra==cfra)
			return;
	}

	if(!psmd->dm)
		return;

	/* baked path softbody */
	if(psys->part->type==PART_HAIR && psys->soft)
		psys_to_softbody(ob, psys, 0);

	/* not needed, this is all handled in hair_step */
	///* is the mesh changing under the edited particles? */
	//if((psys->flag & PSYS_EDITED) &&  psys->part->type==PART_HAIR && psys->recalc & PSYS_RECALC_HAIR) {
	//	/* Just update the particles on the mesh */
	//	psys_update_edithair_dmfaces(ob, psmd->dm, psys);
	//}
	
	if(psys->part->type==PART_HAIR && hair_needs_recalc(psys)){
		float hcfra=0.0f;
		int i;
		free_hair(psys);

		/* first step is negative so particles get killed and reset */
		psys->cfra=1.0f;

		for(i=0; i<=psys->part->hair_step; i++){
			hcfra=100.0f*(float)i/(float)psys->part->hair_step;
			system_step(ob,psys,psmd,hcfra);
			save_hair(ob,psys,psmd,hcfra);
		}

		psys->flag |= PSYS_HAIR_DONE;

		if(psys->softflag&OB_SB_ENABLE)
			psys_to_softbody(ob,psys,1);
	}

	system_step(ob,psys,psmd,cfra);

	Mat4Invert(psys->imat, ob->obmat);	/* used for duplicators */
}

