#ifndef GNUTLS_CERT_H
# define GNUTLS_CERT_H

#include <gnutls_pk.h>

#include <gnutls_ui.h>

typedef struct {
	MPI *params;		/* the size of params depends on the public 
				 * key algorithm 
				 */
	PKAlgorithm subject_pk_algorithm;

	gnutls_DN  cert_info;
	gnutls_DN  issuer_info;
	opaque	   subjectAltName[X509_CN_SIZE];
	int 	   subjectAltName_size;
	
	opaque	   signature[1024];
	int	   signature_size;
	
	time_t	   expiration_time;
	time_t	   activation_time;

	int	   version; /* 1,2,3 
 	                     */
 	
 	uint8	   keyUsage; /* bits from X509KEY_* 
 	                      */
 	
	int        valid; /* 0 if the certificate looks good.
	                   */

	int        CA;    /* 0 if the certificate does not belong to
	                   * a certificate authority. 1 otherwise.
	                   */
	gnutls_datum raw; /* the raw certificate */
} gnutls_cert;

typedef struct {
	MPI *params;		/* the size of params depends on the public 
				 * key algorithm 
				 */
	PKAlgorithm pk_algorithm;

	gnutls_datum raw; /* the raw key */
} gnutls_private_key;


int _gnutls_cert_supported_kx(gnutls_cert* cert, KXAlgorithm **alg, int *alg_size);
PKAlgorithm _gnutls_map_pk_get_pk(KXAlgorithm kx_algorithm);
int _gnutls_cert2gnutlsCert(gnutls_cert * gCert, gnutls_datum derCert);
gnutls_cert* _gnutls_find_cert( gnutls_cert** cert_list, int cert_list_length, char* name);
int _gnutls_find_cert_list_index(gnutls_cert ** cert_list,
			       int cert_list_length, char *name);

#define MAX_INT_DIGITS 4
void _gnutls_int2str(int k, char* data);


#endif
