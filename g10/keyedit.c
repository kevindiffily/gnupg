/* keyedit.c - keyedit stuff
 *	Copyright (C) 1998 Free Software Foundation, Inc.
 *
 * This file is part of GNUPG.
 *
 * GNUPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GNUPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>

#include "options.h"
#include "packet.h"
#include "errors.h"
#include "iobuf.h"
#include "keydb.h"
#include "memory.h"
#include "util.h"
#include "main.h"
#include "trustdb.h"
#include "filter.h"
#include "ttyio.h"
#include "status.h"
#include "i18n.h"

static void show_prefs( KBNODE keyblock, PKT_user_id *uid );
static void show_key_with_all_names( KBNODE keyblock,
	    int only_marked, int with_fpr, int with_subkeys, int with_prefs );
static void show_key_and_fingerprint( KBNODE keyblock );
static void show_fingerprint( PKT_public_key *pk );
static int menu_adduid( KBNODE keyblock, KBNODE sec_keyblock );
static void menu_deluid( KBNODE pub_keyblock, KBNODE sec_keyblock );
static void menu_delkey( KBNODE pub_keyblock, KBNODE sec_keyblock );
static int menu_select_uid( KBNODE keyblock, int index );
static int menu_select_key( KBNODE keyblock, int index );
static int count_uids( KBNODE keyblock );
static int count_uids_with_flag( KBNODE keyblock, unsigned flag );
static int count_keys_with_flag( KBNODE keyblock, unsigned flag );
static int count_selected_uids( KBNODE keyblock );
static int count_selected_keys( KBNODE keyblock );

#define CONTROL_D ('D' - 'A' + 1)

#define NODFLG_BADSIG (1<<0)  /* bad signature */
#define NODFLG_NOKEY  (1<<1)  /* no public key */
#define NODFLG_SIGERR (1<<2)  /* other sig error */

#define NODFLG_MARK_A (1<<4)  /* temporary mark */

#define NODFLG_SELUID (1<<8)  /* indicate the selected userid */
#define NODFLG_SELKEY (1<<9)  /* indicate the selected key */


static int
get_keyblock_byname( KBNODE *keyblock, KBPOS *kbpos, const char *username )
{
    int rc;

    *keyblock = NULL;
    /* search the userid */
    rc = find_keyblock_byname( kbpos, username );
    if( rc ) {
	log_error(_("%s: user not found\n"), username );
	return rc;
    }

    /* read the keyblock */
    rc = read_keyblock( kbpos, keyblock );
    if( rc )
	log_error("%s: keyblock read problem: %s\n", username, g10_errstr(rc));
    else
	merge_keys_and_selfsig( *keyblock );

    return rc;
}


/****************
 * Check the keysigs and set the flags to indicate errors.
 * Returns true if error found.
 */
static int
check_all_keysigs( KBNODE keyblock, int only_selected )
{
    KBNODE kbctx;
    KBNODE node;
    int rc;
    int inv_sigs = 0;
    int no_key = 0;
    int oth_err = 0;
    int has_selfsig = 0;
    int mis_selfsig = 0;
    int selected = !only_selected;
    int anyuid = 0;

    for( kbctx=NULL; (node=walk_kbnode( keyblock, &kbctx, 0)) ; ) {
	if( node->pkt->pkttype == PKT_USER_ID ) {
	    PKT_user_id *uid = node->pkt->pkt.user_id;

	    if( only_selected )
		selected = (node->flag & NODFLG_SELUID);
	    if( selected ) {
		tty_printf("uid  ");
		tty_print_string( uid->name, uid->len );
		tty_printf("\n");
		if( anyuid && !has_selfsig )
		    mis_selfsig++;
		has_selfsig = 0;
		anyuid = 1;
	    }
	}
	else if( selected && node->pkt->pkttype == PKT_SIGNATURE
		 && (node->pkt->pkt.signature->sig_class&~3) == 0x10 ) {
	    PKT_signature *sig = node->pkt->pkt.signature;
	    int sigrc, selfsig;

	    switch( (rc = check_key_signature( keyblock, node, &selfsig)) ) {
	      case 0:
		node->flag &= ~(NODFLG_BADSIG|NODFLG_NOKEY|NODFLG_SIGERR);
		sigrc = '!';
		break;
	      case G10ERR_BAD_SIGN:
		node->flag = NODFLG_BADSIG;
		sigrc = '-';
		inv_sigs++;
		break;
	      case G10ERR_NO_PUBKEY:
		node->flag = NODFLG_NOKEY;
		sigrc = '?';
		no_key++;
		break;
	      default:
		node->flag = NODFLG_SIGERR;
		sigrc = '%';
		oth_err++;
		break;
	    }
	    if( sigrc != '?' ) {
		tty_printf("sig%c       %08lX %s   ",
			sigrc, sig->keyid[1], datestr_from_sig(sig));
		if( sigrc == '%' )
		    tty_printf("[%s] ", g10_errstr(rc) );
		else if( sigrc == '?' )
		    ;
		else if( selfsig ) {
		    tty_printf( _("[self-signature]") );
		    if( sigrc == '!' )
			has_selfsig = 1;
		}
		else {
		    size_t n;
		    char *p = get_user_id( sig->keyid, &n );
		    tty_print_string( p, n > 40? 40 : n );
		    m_free(p);
		}
		tty_printf("\n");
		/* fixme: Should we update the trustdb here */
	    }
	}
    }
    if( !has_selfsig )
	mis_selfsig++;
    if( inv_sigs == 1 )
	tty_printf(_("1 bad signature\n"), inv_sigs );
    else if( inv_sigs )
	tty_printf(_("%d bad signatures\n"), inv_sigs );
    if( no_key == 1 )
	tty_printf(_("1 signature not checked due to a missing key\n") );
    else if( no_key )
	tty_printf(_("%d signatures not checked due to missing keys\n"), no_key );
    if( oth_err == 1 )
	tty_printf(_("1 signature not checked due to an error\n") );
    else if( oth_err )
	tty_printf(_("%d signatures not checked due to errors\n"), oth_err );
    if( mis_selfsig == 1 )
	tty_printf(_("1 user id without valid self-signature detected\n"));
    else if( mis_selfsig  )
	tty_printf(_("%d user ids without valid self-signatures detected\n"),
								    mis_selfsig);

    return inv_sigs || no_key || oth_err || mis_selfsig;
}


/****************
 * Loop over all locusr and and sign the uids after asking.
 * If no user id is marked, all user ids will be signed;
 * if some user_ids are marked those will be signed.
 *
 * fixme: Add support for our proposed sign-all scheme
 */
static int
sign_uids( KBNODE keyblock, STRLIST locusr, int *ret_modified )
{
    int rc = 0;
    SK_LIST sk_list = NULL;
    SK_LIST sk_rover = NULL;
    KBNODE node, uidnode;
    PKT_public_key *primary_pk;
    int select_all = !count_selected_uids(keyblock);
    int upd_trust = 0;

    /* build a list of all signators */
    rc=build_sk_list( locusr, &sk_list, 0, 1 );
    if( rc )
	goto leave;

    /* loop over all signaturs */
    for( sk_rover = sk_list; sk_rover; sk_rover = sk_rover->next ) {
	u32 sk_keyid[2];
	size_t n;
	char *p;

	keyid_from_sk( sk_rover->sk, sk_keyid );
	/* set mark A for all selected user ids */
	for( node=keyblock; node; node = node->next ) {
	    if( select_all || (node->flag & NODFLG_SELUID) )
		node->flag |= NODFLG_MARK_A;
	    else
		node->flag &= ~NODFLG_MARK_A;
	}
	/* reset mark for uids which are already signed */
	uidnode = NULL;
	for( node=keyblock; node; node = node->next ) {
	    if( node->pkt->pkttype == PKT_USER_ID ) {
		uidnode = (node->flag & NODFLG_MARK_A)? node : NULL;
	    }
	    else if( uidnode && node->pkt->pkttype == PKT_SIGNATURE
		&& (node->pkt->pkt.signature->sig_class&~3) == 0x10 ) {
		if( sk_keyid[0] == node->pkt->pkt.signature->keyid[0]
		    && sk_keyid[1] == node->pkt->pkt.signature->keyid[1] ) {
		    tty_printf(_("Already signed by key %08lX\n"),
							(ulong)sk_keyid[1] );
		    uidnode->flag &= ~NODFLG_MARK_A; /* remove mark */
		}
	    }
	}
	/* check whether any uids are left for signing */
	if( !count_uids_with_flag(keyblock, NODFLG_MARK_A) ) {
	    tty_printf(_("Nothing to sign with key %08lX\n"),
						  (ulong)sk_keyid[1] );
	    continue;
	}
	/* Ask whether we really should sign these user id(s) */
	tty_printf("\n");
	show_key_with_all_names( keyblock, 1, 1, 0, 0 );
	tty_printf(_(
	     "Are you really sure that you want to sign this key\n"
	     "with your key: \""));
	p = get_user_id( sk_keyid, &n );
	tty_print_string( p, n );
	m_free(p); p = NULL;
	tty_printf("\"\n\n");

	if( !cpr_get_answer_is_yes(N_("sign_uid.okay"), _("Really sign? ")) )
	    continue;;
	/* now we can sign the user ids */
      reloop: /* (must use this, because we are modifing the list) */
	primary_pk = NULL;
	for( node=keyblock; node; node = node->next ) {
	    if( node->pkt->pkttype == PKT_PUBLIC_KEY )
		primary_pk = node->pkt->pkt.public_key;
	    else if( node->pkt->pkttype == PKT_USER_ID
		     && (node->flag & NODFLG_MARK_A) ) {
		PACKET *pkt;
		PKT_signature *sig;

		assert( primary_pk );
		node->flag &= ~NODFLG_MARK_A;
		rc = make_keysig_packet( &sig, primary_pk,
					       node->pkt->pkt.user_id,
					       NULL,
					       sk_rover->sk,
					       0x10, 0, NULL, NULL );
		if( rc ) {
		    log_error(_("signing failed: %s\n"), g10_errstr(rc));
		    goto leave;
		}
		*ret_modified = 1; /* we changed the keyblock */
		upd_trust = 1;

		pkt = m_alloc_clear( sizeof *pkt );
		pkt->pkttype = PKT_SIGNATURE;
		pkt->pkt.signature = sig;
		insert_kbnode( node, new_kbnode(pkt), PKT_SIGNATURE );
		goto reloop;
	    }
	}
    } /* end loop over signators */
    if( upd_trust && primary_pk ) {
	rc = clear_trust_checked_flag( primary_pk );
    }


  leave:
    release_sk_list( sk_list );
    return rc;
}



/****************
 * Change the passphrase of the primary and all secondary keys.
 * We use only one passphrase for all keys.
 */
static int
change_passphrase( KBNODE keyblock )
{
    int rc = 0;
    int changed=0;
    KBNODE node;
    PKT_secret_key *sk;
    char *passphrase = NULL;

    node = find_kbnode( keyblock, PKT_SECRET_KEY );
    if( !node ) {
	log_error("Oops; secret key not found anymore!\n");
	goto leave;
    }
    sk = node->pkt->pkt.secret_key;

    switch( is_secret_key_protected( sk ) ) {
      case -1:
	rc = G10ERR_PUBKEY_ALGO;
	break;
      case 0:
	tty_printf(_("This key is not protected.\n"));
	break;
      default:
	tty_printf(_("Key is protected.\n"));
	rc = check_secret_key( sk, 0 );
	if( !rc )
	    passphrase = get_last_passphrase();
	break;
    }

    /* unprotect all subkeys (use the supplied passphrase or ask)*/
    for(node=keyblock; !rc && node; node = node->next ) {
	if( node->pkt->pkttype == PKT_SECRET_SUBKEY ) {
	    PKT_secret_key *subsk = node->pkt->pkt.secret_key;
	    set_next_passphrase( passphrase );
	    rc = check_secret_key( subsk, 0 );
	}
    }

    if( rc )
	tty_printf(_("Can't edit this key: %s\n"), g10_errstr(rc));
    else {
	DEK *dek = NULL;
	STRING2KEY *s2k = m_alloc_secure( sizeof *s2k );

	tty_printf(_("Enter the new passphrase for this secret key.\n\n") );

	set_next_passphrase( NULL );
	for(;;) {
	    s2k->mode = opt.s2k_mode;
	    s2k->hash_algo = opt.s2k_digest_algo;
	    dek = passphrase_to_dek( NULL, opt.s2k_cipher_algo, s2k, 2 );
	    if( !dek ) {
		tty_printf(_("passphrase not correctly repeated; try again.\n"));
	    }
	    else if( !dek->keylen ) {
		rc = 0;
		tty_printf(_( "You don't want a passphrase -"
			    " this is probably a *bad* idea!\n\n"));
		if( cpr_get_answer_is_yes(N_("change_passwd.empty.okay"),
			       _("Do you really want to do this? ")))
		    changed++;
		break;
	    }
	    else { /* okay */
		sk->protect.algo = dek->algo;
		sk->protect.s2k = *s2k;
		rc = protect_secret_key( sk, dek );
		for(node=keyblock; !rc && node; node = node->next ) {
		    if( node->pkt->pkttype == PKT_SECRET_SUBKEY ) {
			PKT_secret_key *subsk = node->pkt->pkt.secret_key;
			subsk->protect.algo = dek->algo;
			subsk->protect.s2k = *s2k;
			rc = protect_secret_key( subsk, dek );
		    }
		}
		if( rc )
		    log_error("protect_secret_key failed: %s\n", g10_errstr(rc) );
		else
		    changed++;
		break;
	    }
	}
	m_free(s2k);
	m_free(dek);
    }

  leave:
    m_free( passphrase );
    set_next_passphrase( NULL );
    return changed && !rc;
}




/****************
 * Menu driven key editor
 *
 * Note: to keep track of some selection we use node->mark MARKBIT_xxxx.
 */

void
keyedit_menu( const char *username, STRLIST locusr )
{
    enum cmdids { cmdNONE = 0,
	   cmdQUIT, cmdHELP, cmdFPR, cmdLIST, cmdSELUID, cmdCHECK, cmdSIGN,
	   cmdDEBUG, cmdSAVE, cmdADDUID, cmdDELUID, cmdADDKEY, cmdDELKEY,
	   cmdTOGGLE, cmdSELKEY, cmdPASSWD, cmdTRUST, cmdPREF,
	   cmdNOP };
    static struct { const char *name;
		    enum cmdids id;
		    int need_sk;
		    const char *desc;
		  } cmds[] = {
	{ N_("quit")    , cmdQUIT   , 0, N_("quit this menu") },
	{ N_("q")       , cmdQUIT   , 0, NULL   },
	{ N_("save")    , cmdSAVE   , 0, N_("save and quit") },
	{ N_("help")    , cmdHELP   , 0, N_("show this help") },
	{    "?"        , cmdHELP   , 0, NULL   },
	{ N_("fpr")     , cmdFPR    , 0, N_("show fingerprint") },
	{ N_("list")    , cmdLIST   , 0, N_("list key and user ids") },
	{ N_("l")       , cmdLIST   , 0, NULL   },
	{ N_("uid")     , cmdSELUID , 0, N_("select user id N") },
	{ N_("key")     , cmdSELKEY , 0, N_("select secondary key N") },
	{ N_("check")   , cmdCHECK  , 0, N_("list signatures") },
	{ N_("c")       , cmdCHECK  , 0, NULL },
	{ N_("sign")    , cmdSIGN   , 0, N_("sign the key") },
	{ N_("s")       , cmdSIGN   , 0, NULL },
	{ N_("debug")   , cmdDEBUG  , 0, NULL },
	{ N_("adduid")  , cmdADDUID , 1, N_("add a user id") },
	{ N_("deluid")  , cmdDELUID , 0, N_("delete user id") },
	{ N_("addkey")  , cmdADDKEY , 1, N_("add a secondary key") },
	{ N_("delkey")  , cmdDELKEY , 0, N_("delete a secondary key") },
	{ N_("toggle")  , cmdTOGGLE , 1, N_("toggle between secret "
					    "and public key listing") },
	{ N_("t"     )  , cmdTOGGLE , 1, NULL },
	{ N_("pref")    , cmdPREF  , 0, N_("list preferences") },
	{ N_("passwd")  , cmdPASSWD , 1, N_("change the passphrase") },
	{ N_("trust")   , cmdTRUST , 0, N_("change the ownertrust") },

    { NULL, cmdNONE } };
    enum cmdids cmd;
    int rc = 0;
    KBNODE keyblock = NULL;
    KBPOS keyblockpos;
    KBNODE sec_keyblock = NULL;
    KBPOS sec_keyblockpos;
    KBNODE cur_keyblock;
    char *answer = NULL;
    int redisplay = 1;
    int modified = 0;
    int sec_modified = 0;
    int toggle;


    if( opt.batch ) {
	log_error(_("can't do that in batchmode\n"));
	goto leave;
    }

    /* first try to locate it as secret key */
    rc = find_secret_keyblock_byname( &sec_keyblockpos, username );
    if( !rc ) {
	rc = read_keyblock( &sec_keyblockpos, &sec_keyblock );
	if( rc ) {
	    log_error("%s: secret keyblock read problem: %s\n",
					    username, g10_errstr(rc));
	    goto leave;
	}
	merge_keys_and_selfsig( sec_keyblock );
    }

    /* and now get the public key */
    rc = get_keyblock_byname( &keyblock, &keyblockpos, username );
    if( rc )
	goto leave;

    if( sec_keyblock ) { /* check that they match */
	/* FIXME: check that they both match */
	tty_printf(_("Secret key is available.\n"));
    }

    toggle = 0;
    cur_keyblock = keyblock;
    for(;;) { /* main loop */
	int i, arg_number;
	char *p;

	tty_printf("\n");
	if( redisplay ) {
	    show_key_with_all_names( cur_keyblock, 0, 0, 1, 0 );
	    tty_printf("\n");
	    redisplay = 0;
	}
	m_free(answer);
	answer = cpr_get(N_("keyedit.cmd"), _("Command> "));
	cpr_kill_prompt();
	trim_spaces(answer);

	arg_number = 0;
	if( !*answer )
	    cmd = cmdLIST;
	else if( *answer == CONTROL_D )
	    cmd = cmdQUIT;
	else if( isdigit( *answer ) ) {
	    cmd = cmdSELUID;
	    arg_number = atoi(answer);
	}
	else {
	    if( (p=strchr(answer,' ')) ) {
		*p++ = 0;
		trim_spaces(answer);
		trim_spaces(p);
		arg_number = atoi(p);
	    }

	    for(i=0; cmds[i].name; i++ )
		if( !stricmp( answer, cmds[i].name ) )
		    break;
	    if( cmds[i].need_sk && !sec_keyblock ) {
		tty_printf(_("Need the secret key to to this.\n"));
		cmd = cmdNOP;
	    }
	    else
		cmd = cmds[i].id;
	}
	switch( cmd )  {
	  case cmdHELP:
	    for(i=0; cmds[i].name; i++ ) {
		if( cmds[i].need_sk && !sec_keyblock )
		    ; /* skip if we do not have the secret key */
		else if( cmds[i].desc )
		    tty_printf("%-10s %s\n", cmds[i].name, _(cmds[i].desc) );
	    }
	    break;

	  case cmdQUIT:
	    if( !modified && !sec_modified )
		goto leave;
	    if( !cpr_get_answer_is_yes(N_("keyedit.save.okay"),
					_("Save changes? ")) ) {
		if( cpr_enabled()
		    || cpr_get_answer_is_yes(N_("keyedit.cancel.okay"),
					     _("Quit without saving? ")) )
		    goto leave;
		break;
	    }
	    /* fall thru */
	  case cmdSAVE:
	    if( modified || sec_modified  ) {
		if( modified ) {
		    rc = update_keyblock( &keyblockpos, keyblock );
		    if( rc ) {
			log_error(_("update failed: %s\n"), g10_errstr(rc) );
			break;
		    }
		}
		if( sec_modified ) {
		    rc = update_keyblock( &sec_keyblockpos, sec_keyblock );
		    if( rc ) {
			log_error(_("update secret failed: %s\n"),
							    g10_errstr(rc) );
			break;
		    }
		}
		/* FIXME: UPDATE/INVALIDATE trustdb !! */
	    }
	    else
		tty_printf(_("Key not changed so no update needed.\n"));
	    goto leave;

	  case cmdLIST:
	    redisplay = 1;
	    break;

	  case cmdFPR:
	    show_key_and_fingerprint( keyblock );
	    break;

	  case cmdSELUID:
	    if( menu_select_uid( cur_keyblock, arg_number ) )
		redisplay = 1;
	    break;

	  case cmdSELKEY:
	    if( menu_select_key( cur_keyblock, arg_number ) )
		redisplay = 1;
	    break;

	  case cmdCHECK:
	    /* we can only do this with the public key becuase the
	     * check functions can't cope with secret keys and it
	     * is questionable whether this would make sense at all */
	    check_all_keysigs( keyblock, count_selected_uids(keyblock) );
	    break;

	  case cmdSIGN: /* sign (only the public key) */
	    if( count_uids(keyblock) > 1 && !count_selected_uids(keyblock) ) {
		if( !cpr_get_answer_is_yes(N_("keyedit.sign_all.okay"),
					   _("Really sign all user ids? ")) ) {
		    tty_printf(_("Hint: Select the user ids to sign\n"));
		    break;
		}
	    }
	    sign_uids( keyblock, locusr, &modified );
	    break;

	  case cmdDEBUG:
	    dump_kbnode( cur_keyblock );
	    break;

	  case cmdTOGGLE:
	    toggle = !toggle;
	    cur_keyblock = toggle? sec_keyblock : keyblock;
	    redisplay = 1;
	    break;

	  case cmdADDUID:
	    if( menu_adduid( keyblock, sec_keyblock ) ) {
		redisplay = 1;
		sec_modified = modified = 1;
	    }
	    break;

	  case cmdDELUID: {
		int n1;

		if( !(n1=count_selected_uids(keyblock)) )
		    tty_printf(_("You must select at least one user id.\n"));
		else if( count_uids(keyblock) - n1 < 1 )
		    tty_printf(_("You can't delete the last user id!\n"));
		else if( cpr_get_answer_is_yes(
			    N_("keyedit.remove.uid.okay"),
			n1 > 1? _("Really remove all selected user ids? ")
			      : _("Really remove this user id? ")
		       ) ) {
		    menu_deluid( keyblock, sec_keyblock );
		    redisplay = 1;
		    modified = 1;
		    if( sec_keyblock )
		       sec_modified = 1;
		}
	    }
	    break;

	  case cmdADDKEY:
	    if( generate_subkeypair( keyblock, sec_keyblock ) ) {
		redisplay = 1;
		sec_modified = modified = 1;
	    }
	    break;


	  case cmdDELKEY: {
		int n1;

		if( !(n1=count_selected_keys( keyblock )) )
		    tty_printf(_("You must select at least one key.\n"));
		else if( sec_keyblock && !cpr_get_answer_is_yes(
			    N_("keyedit.remove.subkey.okay"),
		       n1 > 1?
			_("Do you really want to delete the selected keys? "):
			_("Do you really want to delete this key? ")
		       ))
		    ;
		else {
		    menu_delkey( keyblock, sec_keyblock );
		    redisplay = 1;
		    modified = 1;
		    if( sec_keyblock )
		       sec_modified = 1;
		}
	    }
	    break;

	  case cmdPASSWD:
	    if( change_passphrase( sec_keyblock ) )
		sec_modified = 1;
	    break;

	  case cmdTRUST:
	    show_key_with_all_names( keyblock, 0, 0, 1, 0 );
	    tty_printf("\n");
	    if( edit_ownertrust( find_kbnode( keyblock,
		      PKT_PUBLIC_KEY )->pkt->pkt.public_key->local_id, 1 ) )
		redisplay = 1;
	    /* we don't need to set modified here, as the trustvalues
	     * are updated immediately */
	    break;

	  case cmdPREF:
	    show_key_with_all_names( keyblock, 0, 0, 0, 1 );
	    break;

	  case cmdNOP:
	    break;

	  default:
	    tty_printf("\n");
	    tty_printf(_("Invalid command  (try \"help\")\n"));
	    break;
	}
    } /* end main loop */

  leave:
    release_kbnode( keyblock );
    release_kbnode( sec_keyblock );
    m_free(answer);
}


/****************
 * show preferences of a public keyblock.
 */
static void
show_prefs( KBNODE keyblock, PKT_user_id *uid )
{
    KBNODE node = find_kbnode( keyblock, PKT_PUBLIC_KEY );
    PKT_public_key *pk;
    byte *p;
    int i;
    size_t n;
    byte namehash[20];

    if( !node )
	return; /* is a secret keyblock */
    pk = node->pkt->pkt.public_key;
    if( !pk->local_id ) {
	log_error("oops: no LID\n");
	return;
    }

    rmd160_hash_buffer( namehash, uid->name, uid->len );

    p = get_pref_data( pk->local_id, namehash, &n );
    if( !p )
	return;

    tty_printf("    ");
    for(i=0; i < n; i+=2 ) {
	if( p[i] )
	    tty_printf( " %c%d", p[i] == PREFTYPE_SYM    ? 'S' :
				 p[i] == PREFTYPE_HASH	 ? 'H' :
				 p[i] == PREFTYPE_COMPR  ? 'Z' : '?', p[i+1]);
    }
    tty_printf("\n");

    m_free(p);
}


/****************
 * Display the key a the user ids, if only_marked is true, do only
 * so for user ids with mark A flag set and dont display the index number
 */
static void
show_key_with_all_names( KBNODE keyblock, int only_marked,
			 int with_fpr, int with_subkeys, int with_prefs )
{
    KBNODE node;
    int i;

    /* the keys */
    for( node = keyblock; node; node = node->next ) {
	if( node->pkt->pkttype == PKT_PUBLIC_KEY
	    || (with_subkeys && node->pkt->pkttype == PKT_PUBLIC_SUBKEY) ) {
	    PKT_public_key *pk = node->pkt->pkt.public_key;
	    int otrust=0, trust=0;

	    if( node->pkt->pkttype == PKT_PUBLIC_KEY ) {
		/* do it here, so that debug messages don't clutter the
		 * output */
		trust = query_trust_info(pk);
		otrust = get_ownertrust_info( pk->local_id );
	    }

	    tty_printf("%s%c %4u%c/%08lX  created: %s expires: %s",
			  node->pkt->pkttype == PKT_PUBLIC_KEY? "pub":"sub",
			  (node->flag & NODFLG_SELKEY)? '*':' ',
			  nbits_from_pk( pk ),
			  pubkey_letter( pk->pubkey_algo ),
			  (ulong)keyid_from_pk(pk,NULL),
			  datestr_from_pk(pk),
			  expirestr_from_pk(pk) );
	    if( node->pkt->pkttype == PKT_PUBLIC_KEY ) {
		tty_printf(" trust: %c/%c", otrust, trust );
		if( with_fpr  )
		    show_fingerprint( pk );
	    }
	    tty_printf("\n");
	}
	else if( node->pkt->pkttype == PKT_SECRET_KEY
	    || (with_subkeys && node->pkt->pkttype == PKT_SECRET_SUBKEY) ) {
	    PKT_secret_key *sk = node->pkt->pkt.secret_key;
	    tty_printf("%s%c %4u%c/%08lX  created: %s expires: %s\n",
			  node->pkt->pkttype == PKT_SECRET_KEY? "sec":"sbb",
			  (node->flag & NODFLG_SELKEY)? '*':' ',
			  nbits_from_sk( sk ),
			  pubkey_letter( sk->pubkey_algo ),
			  (ulong)keyid_from_sk(sk,NULL),
			  datestr_from_sk(sk),
			  expirestr_from_sk(sk) );
	}
    }
    /* the user ids */
    i = 0;
    for( node = keyblock; node; node = node->next ) {
	if( node->pkt->pkttype == PKT_USER_ID ) {
	    PKT_user_id *uid = node->pkt->pkt.user_id;
	    ++i;
	    if( !only_marked || (only_marked && (node->flag & NODFLG_MARK_A))){
		if( only_marked )
		   tty_printf("     ");
		else if( node->flag & NODFLG_SELUID )
		   tty_printf("(%d)* ", i);
		else
		   tty_printf("(%d)  ", i);
		tty_print_string( uid->name, uid->len );
		tty_printf("\n");
		if( with_prefs )
		    show_prefs( keyblock, uid );
	    }
	}
    }
}

static void
show_key_and_fingerprint( KBNODE keyblock )
{
    KBNODE node;
    PKT_public_key *pk = NULL;

    for( node = keyblock; node; node = node->next ) {
	if( node->pkt->pkttype == PKT_PUBLIC_KEY ) {
	    pk = node->pkt->pkt.public_key;
	    tty_printf("pub  %4u%c/%08lX %s ",
			  nbits_from_pk( pk ),
			  pubkey_letter( pk->pubkey_algo ),
			  (ulong)keyid_from_pk(pk,NULL),
			  datestr_from_pk(pk) );
	}
	else if( node->pkt->pkttype == PKT_USER_ID ) {
	    PKT_user_id *uid = node->pkt->pkt.user_id;
	    tty_print_string( uid->name, uid->len );
	    break;
	}
    }
    tty_printf("\n");
    if( pk )
	show_fingerprint( pk );
}


static void
show_fingerprint( PKT_public_key *pk )
{
    byte *array, *p;
    size_t i, n;

    p = array = fingerprint_from_pk( pk, NULL, &n );
    tty_printf("             Fingerprint:");
    if( n == 20 ) {
	for(i=0; i < n ; i++, i++, p += 2 ) {
	    if( i == 10 )
		tty_printf(" ");
	    tty_printf(" %02X%02X", *p, p[1] );
	}
    }
    else {
	for(i=0; i < n ; i++, p++ ) {
	    if( i && !(i%8) )
		tty_printf(" ");
	    tty_printf(" %02X", *p );
	}
    }
    tty_printf("\n");
    m_free(array);
}


/****************
 * Ask for a new user id , do the selfsignature and put it into
 * both keyblocks.
 * Return true if there is a new user id
 */
static int
menu_adduid( KBNODE pub_keyblock, KBNODE sec_keyblock )
{
    PKT_user_id *uid;
    PKT_public_key *pk=NULL;
    PKT_secret_key *sk=NULL;
    PKT_signature *sig=NULL;
    PACKET *pkt;
    KBNODE node;
    KBNODE pub_where=NULL, sec_where=NULL;
    int rc;

    uid = generate_user_id();
    if( !uid )
	return 0;

    for( node = pub_keyblock; node; pub_where = node, node = node->next ) {
	if( node->pkt->pkttype == PKT_PUBLIC_KEY )
	    pk = node->pkt->pkt.public_key;
	else if( node->pkt->pkttype == PKT_PUBLIC_SUBKEY )
	    break;
    }
    if( !node ) /* no subkey */
	pub_where = NULL;
    for( node = sec_keyblock; node; sec_where = node, node = node->next ) {
	if( node->pkt->pkttype == PKT_SECRET_KEY )
	    sk = node->pkt->pkt.secret_key;
	else if( node->pkt->pkttype == PKT_SECRET_SUBKEY )
	    break;
    }
    if( !node ) /* no subkey */
	sec_where = NULL;
    assert(pk && sk );

    rc = make_keysig_packet( &sig, pk, uid, NULL, sk, 0x13, 0,
			     keygen_add_std_prefs, sk );
    if( rc ) {
	log_error("signing failed: %s\n", g10_errstr(rc) );
	free_user_id(uid);
	return 0;
    }

    /* insert/append to secret keyblock */
    pkt = m_alloc_clear( sizeof *pkt );
    pkt->pkttype = PKT_USER_ID;
    pkt->pkt.user_id = copy_user_id(NULL, uid);
    node = new_kbnode(pkt);
    if( sec_where )
	insert_kbnode( sec_where, node, 0 );
    else
	add_kbnode( sec_keyblock, node );
    pkt = m_alloc_clear( sizeof *pkt );
    pkt->pkttype = PKT_SIGNATURE;
    pkt->pkt.signature = copy_signature(NULL, sig);
    if( sec_where )
	insert_kbnode( node, new_kbnode(pkt), 0 );
    else
	add_kbnode( sec_keyblock, new_kbnode(pkt) );
    /* insert/append to public keyblock */
    pkt = m_alloc_clear( sizeof *pkt );
    pkt->pkttype = PKT_USER_ID;
    pkt->pkt.user_id = uid;
    node = new_kbnode(pkt);
    if( pub_where )
	insert_kbnode( pub_where, node, 0 );
    else
	add_kbnode( pub_keyblock, node );
    pkt = m_alloc_clear( sizeof *pkt );
    pkt->pkttype = PKT_SIGNATURE;
    pkt->pkt.signature = copy_signature(NULL, sig);
    if( pub_where )
	insert_kbnode( node, new_kbnode(pkt), 0 );
    else
	add_kbnode( pub_keyblock, new_kbnode(pkt) );
    return 1;
}


/****************
 * Remove all selceted userids from the keyrings
 */
static void
menu_deluid( KBNODE pub_keyblock, KBNODE sec_keyblock )
{
    KBNODE node;
    int selected=0;

    for( node = pub_keyblock; node; node = node->next ) {
	if( node->pkt->pkttype == PKT_USER_ID ) {
	    selected = node->flag & NODFLG_SELUID;
	    if( selected ) {
		delete_kbnode( node );
		if( sec_keyblock ) {
		    KBNODE snode;
		    int s_selected = 0;
		    PKT_user_id *uid = node->pkt->pkt.user_id;
		    for( snode = sec_keyblock; snode; snode = snode->next ) {
			if( snode->pkt->pkttype == PKT_USER_ID ) {
			    PKT_user_id *suid = snode->pkt->pkt.user_id;

			    s_selected =
				(uid->len == suid->len
				 && !memcmp( uid->name, suid->name, uid->len));
			    if( s_selected )
				delete_kbnode( snode );
			}
			else if( s_selected
				 && snode->pkt->pkttype == PKT_SIGNATURE )
			    delete_kbnode( snode );
			else if( snode->pkt->pkttype == PKT_SECRET_SUBKEY )
			    s_selected = 0;
		    }
		}
	    }
	}
	else if( selected && node->pkt->pkttype == PKT_SIGNATURE )
	    delete_kbnode( node );
	else if( node->pkt->pkttype == PKT_PUBLIC_SUBKEY )
	    selected = 0;
    }
    commit_kbnode( &pub_keyblock );
    if( sec_keyblock )
	commit_kbnode( &sec_keyblock );
}


/****************
 * Remove some of the secondary keys
 */
static void
menu_delkey( KBNODE pub_keyblock, KBNODE sec_keyblock )
{
    KBNODE node;
    int selected=0;

    for( node = pub_keyblock; node; node = node->next ) {
	if( node->pkt->pkttype == PKT_PUBLIC_SUBKEY ) {
	    selected = node->flag & NODFLG_SELKEY;
	    if( selected ) {
		delete_kbnode( node );
		if( sec_keyblock ) {
		    KBNODE snode;
		    int s_selected = 0;
		    u32 ki[2];

		    keyid_from_pk( node->pkt->pkt.public_key, ki );
		    for( snode = sec_keyblock; snode; snode = snode->next ) {
			if( snode->pkt->pkttype == PKT_SECRET_SUBKEY ) {
			    u32 ki2[2];

			    keyid_from_sk( snode->pkt->pkt.secret_key, ki2 );
			    s_selected = (ki[0] == ki2[0] && ki[1] == ki2[1]);
			    if( s_selected )
				delete_kbnode( snode );
			}
			else if( s_selected
				 && snode->pkt->pkttype == PKT_SIGNATURE )
			    delete_kbnode( snode );
			else
			    s_selected = 0;
		    }
		}
	    }
	}
	else if( selected && node->pkt->pkttype == PKT_SIGNATURE )
	    delete_kbnode( node );
	else
	    selected = 0;
    }
    commit_kbnode( &pub_keyblock );
    if( sec_keyblock )
	commit_kbnode( &sec_keyblock );
}


/****************
 * Select one user id or remove all selection if index is 0.
 * Returns: True if the selection changed;
 */
static int
menu_select_uid( KBNODE keyblock, int index )
{
    KBNODE node;
    int i;

    /* first check that the index is valid */
    if( index ) {
	for( i=0, node = keyblock; node; node = node->next ) {
	    if( node->pkt->pkttype == PKT_USER_ID ) {
		if( ++i == index )
		    break;
	    }
	}
	if( !node ) {
	    tty_printf(_("No user id with index %d\n"), index );
	    return 0;
	}
    }
    else { /* reset all */
	for( i=0, node = keyblock; node; node = node->next ) {
	    if( node->pkt->pkttype == PKT_USER_ID )
		node->flag &= ~NODFLG_SELUID;
	}
	return 1;
    }
    /* and toggle the new index */
    for( i=0, node = keyblock; node; node = node->next ) {
	if( node->pkt->pkttype == PKT_USER_ID ) {
	    if( ++i == index )
		if( (node->flag & NODFLG_SELUID) )
		    node->flag &= ~NODFLG_SELUID;
		else
		    node->flag |= NODFLG_SELUID;
	}
    }

    return 1;
}

/****************
 * Select secondary keys
 * Returns: True if the selection changed;
 */
static int
menu_select_key( KBNODE keyblock, int index )
{
    KBNODE node;
    int i;

    /* first check that the index is valid */
    if( index ) {
	for( i=0, node = keyblock; node; node = node->next ) {
	    if( node->pkt->pkttype == PKT_PUBLIC_SUBKEY
		|| node->pkt->pkttype == PKT_SECRET_SUBKEY ) {
		if( ++i == index )
		    break;
	    }
	}
	if( !node ) {
	    tty_printf(_("No secondary key with index %d\n"), index );
	    return 0;
	}
    }
    else { /* reset all */
	for( i=0, node = keyblock; node; node = node->next ) {
	    if( node->pkt->pkttype == PKT_PUBLIC_SUBKEY
		|| node->pkt->pkttype == PKT_SECRET_SUBKEY )
		node->flag &= ~NODFLG_SELKEY;
	}
	return 1;
    }
    /* and set the new index */
    for( i=0, node = keyblock; node; node = node->next ) {
	if( node->pkt->pkttype == PKT_PUBLIC_SUBKEY
	    || node->pkt->pkttype == PKT_SECRET_SUBKEY ) {
	    if( ++i == index )
		if( (node->flag & NODFLG_SELKEY) )
		    node->flag &= ~NODFLG_SELKEY;
		else
		    node->flag |= NODFLG_SELKEY;
	}
    }

    return 1;
}


static int
count_uids_with_flag( KBNODE keyblock, unsigned flag )
{
    KBNODE node;
    int i=0;

    for( node = keyblock; node; node = node->next )
	if( node->pkt->pkttype == PKT_USER_ID && (node->flag & flag) )
	    i++;
    return i;
}

static int
count_keys_with_flag( KBNODE keyblock, unsigned flag )
{
    KBNODE node;
    int i=0;

    for( node = keyblock; node; node = node->next )
	if( ( node->pkt->pkttype == PKT_PUBLIC_SUBKEY
	      || node->pkt->pkttype == PKT_SECRET_SUBKEY)
	    && (node->flag & flag) )
	    i++;
    return i;
}

static int
count_uids( KBNODE keyblock )
{
    KBNODE node;
    int i=0;

    for( node = keyblock; node; node = node->next )
	if( node->pkt->pkttype == PKT_USER_ID )
	    i++;
    return i;
}


/****************
 * Returns true if there is at least one selected user id
 */
static int
count_selected_uids( KBNODE keyblock )
{
    return count_uids_with_flag( keyblock, NODFLG_SELUID);
}

static int
count_selected_keys( KBNODE keyblock )
{
    return count_keys_with_flag( keyblock, NODFLG_SELKEY);
}

