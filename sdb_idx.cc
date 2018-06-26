/* Copyright (c) 2018, SequoiaDB and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef MYSQL_SERVER
   #define MYSQL_SERVER
#endif

#include "sdb_idx.h"
#include "sdb_cl.h"
#include "sdb_err_code.h"
#include "sdb_def.h"
#include "sdb_log.h"
#include "sql_table.h"
#include "include/bson/bsonDecimal.h"

BOOLEAN is_field_indexable( const Field *field )
{
   switch( field->type() )
   {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE:
      case MYSQL_TYPE_LONGLONG:
         return TRUE ;
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_STRING:
      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_TINY_BLOB:
      case MYSQL_TYPE_MEDIUM_BLOB:
      case MYSQL_TYPE_LONG_BLOB:
      case MYSQL_TYPE_BLOB:
         {
            if ( !field->binary() ) 
               return TRUE ;
            else 
               return FALSE ;
         }
      default:
         return FALSE ;
   }
}

int sdb_create_index( const KEY *keyInfo, sdb_cl_auto_ptr cl )
{
   const KEY_PART_INFO *keyPart ;
   const KEY_PART_INFO *keyEnd ;
   int rc = 0 ;
   bson::BSONObj keyObj ;
   BOOLEAN isUnique=FALSE, isEnforced=FALSE ;

   bson::BSONObjBuilder keyObjBuilder ;
   keyPart = keyInfo->key_part ;
   keyEnd = keyPart + keyInfo->user_defined_key_parts ;
   for( ; keyPart != keyEnd ; ++keyPart )
   {
      if ( !is_field_indexable( keyPart->field ) )
      {
         rc = HA_ERR_UNSUPPORTED ;
         SDB_PRINT_ERROR( rc,
                          "column '%-.192s' cannot be used in key specification.",
                          keyPart->field->field_name ) ;
         goto error ;
      }
      // TODO: ASC or DESC
      keyObjBuilder.append( keyPart->field->field_name,
                            1 ) ;
   }
   keyObj = keyObjBuilder.obj() ;

   if ( !strcmp( keyInfo->name, primary_key_name ))
   {
      isUnique = TRUE ;
      isEnforced = TRUE ;
   }

   if ( keyInfo->flags & HA_NOSAME )
   {
      isUnique = TRUE ;
   }

   rc = cl->create_index( keyObj, keyInfo->name, isUnique, isEnforced ) ;
   if ( rc )
   {
      goto error ;
   }
done:
   return rc ;
error :
   goto done ;
}

int sdb_drop_index( const KEY *keyInfo, sdb_cl_auto_ptr cl )
{
   int rc = 0 ;
   rc = cl->drop_index( keyInfo->name ) ;
   if ( rc )
   {
      goto error ;
   }
done:
   return rc ;
error :
   goto done ;
}

const char * sdb_get_idx_name( KEY * key_info )
{
   if ( key_info )
   {
      return key_info->name ;
   }
   return NULL ;
}

int sdb_get_idx_order( KEY * key_info, bson::BSONObj &order )
{
   int rc = SDB_ERR_OK ;
   const KEY_PART_INFO *keyPart ;
   const KEY_PART_INFO *keyEnd ;
   bson::BSONObjBuilder obj_builder ;
   if ( !key_info )
   {
      rc = SDB_ERR_INVALID_ARG ;
      goto error ;
   }
   keyPart = key_info->key_part ;
   keyEnd = keyPart + key_info->user_defined_key_parts ;
   for( ; keyPart != keyEnd ; ++keyPart )
   {
      obj_builder.append( keyPart->field->field_name, 1 ) ;
   }
   order = obj_builder.obj() ;

done:
   return rc ;
error:
   goto done ;
}

typedef union _sdb_key_common_type
{
   char     sz_data[8] ;
   int8     int8_val ;
   uint8    uint8_val ;
   int16    int16_val ;
   uint16   uint16_val ;
   int32    int24_val ; 
   uint32   uint24_val ;
   int32    int32_val ;
   uint32   uint32_val ;
   int64    int64_val ;
   uint64   uint64_val ;
}sdb_key_common_type ;

void get_unsigned_key_val( const uchar *key_ptr,
                           key_part_map key_part_map_val,
                           const KEY_PART_INFO *key_part,
                           bson::BSONObjBuilder &obj_builder,
                           const char *op_str )
{
   sdb_key_common_type val_tmp ;
   val_tmp.uint64_val = 0 ;
   if ( key_part->length > sizeof( val_tmp ) )
   {
      goto done ;
   }

   if( key_part_map_val & 1  && ( !key_part->null_bit || 0 == *key_ptr )) 
   { 
      memcpy( &(val_tmp.sz_data[0]),
              key_ptr+key_part->store_length-key_part->length, 
              key_part->length ); 
      switch( key_part->length )
      {
         case 1:
            {
               obj_builder.append( op_str, val_tmp.uint8_val ) ;
               break ;
            }
         case 2:
            {
               obj_builder.append( op_str, val_tmp.uint16_val ) ;
               break ;
            }
         case 3:
         case 4:
            {
               obj_builder.append( op_str, val_tmp.uint32_val ) ;
               break ;
            }
         case 8:
            {
               if( val_tmp.int64_val >= 0)
               {
                  obj_builder.append( op_str, val_tmp.int64_val ) ;
               }
               else
               {
                  bson::bsonDecimal decimal_tmp ;
                  char buf_tmp[24] = {0} ;
                  sprintf( buf_tmp, "%llu", val_tmp.uint64_val ) ;
                  decimal_tmp.fromString( buf_tmp ) ;
                  obj_builder.append( op_str, decimal_tmp ) ;
               }
               break ;
            }
         default:
            break ;
      }
   }

done:
   return ;
}

void get_unsigned_key_range_obj( const uchar *start_key_ptr,
                                 key_part_map start_key_part_map,
                                 enum ha_rkey_function start_find_flag,
                                 const uchar *end_key_ptr,
                                 key_part_map end_key_part_map,
                                 enum ha_rkey_function end_find_flag,
                                 const KEY_PART_INFO *key_part,
                                 bson::BSONObj &obj )
{
   bson::BSONObjBuilder obj_builder ;
   if ( HA_READ_KEY_EXACT == start_find_flag )
   {
      get_unsigned_key_val( start_key_ptr, start_key_part_map,
                            key_part, obj_builder, "$et" ) ;
   }
   else
   {
      get_unsigned_key_val( start_key_ptr, start_key_part_map,
                            key_part, obj_builder, "$gte" ) ;
      get_unsigned_key_val( end_key_ptr, end_key_part_map,
                            key_part, obj_builder, "$lte" ) ;
   }
   obj = obj_builder.obj() ;
}

void get_signed_key_val( const uchar *key_ptr,
                         key_part_map key_part_map_val,
                         const KEY_PART_INFO *key_part,
                         bson::BSONObjBuilder &obj_builder,
                         const char *op_str )
{
   sdb_key_common_type val_tmp ;
   val_tmp.uint64_val = 0 ;
   if ( key_part->length > sizeof( val_tmp ) )
   {
      goto done ;
   }

   if( key_part_map_val & 1  && ( !key_part->null_bit || 0 == *key_ptr )) 
   { 
      memcpy( &(val_tmp.sz_data[0]),
              key_ptr+key_part->store_length-key_part->length, 
              key_part->length ); 
      switch( key_part->length )
      {
         case 1:
            {
               obj_builder.append( op_str, val_tmp.int8_val ) ;
               break ;
            }
         case 2:
            {
               obj_builder.append( op_str, val_tmp.int16_val ) ;
               break ;
            }
         case 3:
            {
               if ( val_tmp.int32_val & 0X800000 )
               {
                  val_tmp.sz_data[3] = 0XFF ;
               }
               obj_builder.append( op_str, val_tmp.int32_val ) ;
               break ;
            }
         case 4:
            {
               obj_builder.append( op_str, val_tmp.int32_val ) ;
               break ;
            }
         case 8:
            {
                obj_builder.append( op_str, val_tmp.int64_val ) ;
               break ;
            }
         default:
            break ;
      }
   }

done:
   return ;
}

void get_signed_key_range_obj( const uchar *start_key_ptr,
                               key_part_map start_key_part_map,
                               enum ha_rkey_function start_find_flag,
                               const uchar *end_key_ptr,
                               key_part_map end_key_part_map,
                               enum ha_rkey_function end_find_flag,
                               const KEY_PART_INFO *key_part,
                               bson::BSONObj &obj )
{
   bson::BSONObjBuilder obj_builder ;
   if ( HA_READ_KEY_EXACT == start_find_flag )
   {
      get_signed_key_val( start_key_ptr, start_key_part_map,
                          key_part, obj_builder, "$et" ) ;
   }
   else
   {
      get_signed_key_val( start_key_ptr, start_key_part_map,
                          key_part, obj_builder, "$gte" ) ;
      get_signed_key_val( end_key_ptr, end_key_part_map,
                          key_part, obj_builder, "$lte" ) ;
   }
   obj = obj_builder.obj() ;
}

void get_int_key_range_obj( const uchar *start_key_ptr,
                            key_part_map start_key_part_map,
                            enum ha_rkey_function start_find_flag,
                            const uchar *end_key_ptr,
                            key_part_map end_key_part_map,
                            enum ha_rkey_function end_find_flag,
                            const KEY_PART_INFO *key_part,
                            bson::BSONObj &obj )
{
   if ( !((Field_num *)(key_part->field))->unsigned_flag )
   {
      get_signed_key_range_obj( start_key_ptr,
                                start_key_part_map,
                                start_find_flag,
                                end_key_ptr,
                                end_key_part_map,
                                end_find_flag,
                                key_part, obj ) ;
   }
   else
   {
      get_unsigned_key_range_obj( start_key_ptr,
                                  start_key_part_map,
                                  start_find_flag,
                                  end_key_ptr,
                                  end_key_part_map,
                                  end_find_flag,
                                  key_part, obj ) ;
   }
}

void get_text_key_val( const uchar *key_ptr,
                       key_part_map key_part_map_val,
                       const KEY_PART_INFO *key_part,
                       bson::BSONObjBuilder &obj_builder,
                       const char *op_str, int length)
{
   if( key_part_map_val & 1  && ( !key_part->null_bit || 0 == *key_ptr || length)) 
   {
      obj_builder.appendStrWithNoTerminating(op_str, (const char*)key_ptr, length);
   }
   return ;
}



void get_text_key_range_obj( const uchar *start_key_ptr,
                             key_part_map start_key_part_map,
                             enum ha_rkey_function start_find_flag,
                             const uchar *end_key_ptr,
                             key_part_map end_key_part_map,
                             enum ha_rkey_function end_find_flag,
                             const KEY_PART_INFO *key_part,
                             bson::BSONObj &obj )
{
   bson::BSONObjBuilder obj_builder ;
   uchar key_field_str_buf[SDB_IDX_FIELD_SIZE_MAX] = {0};       

   /*we ignore the spaces end of key string which was filled by mysql.*/  
   int key_start_pos = key_part->store_length - key_part->length;
   int pos = key_part->store_length - 1;
   while(pos >= key_start_pos && (' ' == start_key_ptr[pos] || '\0' == start_key_ptr[pos]))
   {
      pos--;
   }

   pos++;
   int length = pos - key_start_pos;
   if(length >= SDB_IDX_FIELD_SIZE_MAX)
   {
      return;
   }

   // for exact match and exclude prefix-index.
   if ( HA_READ_KEY_EXACT == start_find_flag
      && !(key_part->key_part_flag & HA_PART_KEY_SEG && length))
   {
      //TODO: it is exact match if start_key_ptr is same as end_key_ptr.
      /*sdb is sensitive to spaces belong to end string, while mysql is not sensitive
      so we return more results to the HA_READ_KEY_EXACT search.
      'where a = "hello"'
      euqal search in sdb with
      '({a:{$regex:"^hello( ){0,}$"})'
      */ 
      key_field_str_buf[0] = '^';
      memcpy(key_field_str_buf + 1, start_key_ptr + key_start_pos, length);

      /*replace {a:{$et:"hello"}} with {a:{$regex:"^hello( ){0,}$"}}*/
      strncpy((char*)&key_field_str_buf[1 + length], "( ){0,}$", sizeof("( ){0,}$"));
      length += strlen("^") + strlen("( ){0,}$");
      get_text_key_val( key_field_str_buf, start_key_part_map,
                         key_part, obj_builder, "$regex", length);
   }
   else/* Find next rec. after key-record, or part key where a="abcdefg" (a(10), key(a(5)->"abcde")) */
   {
      get_text_key_val( start_key_ptr + key_start_pos, start_key_part_map,
                        key_part, obj_builder, "$gte", length ) ;
      if ( HA_READ_BEFORE_KEY == end_find_flag )
      {
         get_text_key_val( end_key_ptr + key_part->store_length - key_part->length, end_key_part_map,
                           key_part, obj_builder, "$lte", key_part->length );
      }
   }

   obj = obj_builder.obj() ;
}

void get_float_key_val( const uchar *key_ptr,
                        key_part_map key_part_map_val,
                        const KEY_PART_INFO *key_part,
                        bson::BSONObjBuilder &obj_builder,
                        const char *op_str )
{
   if( key_part_map_val & 1  && ( !key_part->null_bit || 0 == *key_ptr )) 
   { 
      if ( 4 == key_part->length )
      {
         float tmp = *((float *)(key_ptr+key_part->store_length-key_part->length)) ;
         obj_builder.append( op_str, tmp ) ;
      }
      else if ( 8 == key_part->length )
      {
         double tmp = *((double *)(key_ptr+key_part->store_length-key_part->length)) ;
         obj_builder.append( op_str, tmp ) ;
      }
   }
}

void get_float_key_range_obj( const uchar *start_key_ptr,
                              key_part_map start_key_part_map,
                              enum ha_rkey_function start_find_flag,
                              const uchar *end_key_ptr,
                              key_part_map end_key_part_map,
                              enum ha_rkey_function end_find_flag,
                              const KEY_PART_INFO *key_part,
                              bson::BSONObj &obj )
{
   bson::BSONObjBuilder obj_builder ;
   if ( HA_READ_KEY_EXACT == start_find_flag )
   {
      get_float_key_val( start_key_ptr, start_key_part_map,
                        key_part, obj_builder, "$et" ) ;
   }
   else
   {
      get_float_key_val( start_key_ptr, start_key_part_map,
                         key_part, obj_builder, "$gte" ) ;
      get_float_key_val( end_key_ptr, end_key_part_map,
                         key_part, obj_builder, "$lte" ) ;
   }

   obj = obj_builder.obj() ;
}

int build_match_obj_by_start_stop_key( uint keynr,
                                       const uchar *key_ptr,
                                       key_part_map keypart_map,
                                       enum ha_rkey_function find_flag,
                                       key_range *end_range,
                                       TABLE *table,
                                       bson::BSONObj &matchObj )
{
   int rc = 0 ;
   KEY *keyInfo ;
   const KEY_PART_INFO *keyPart ;
   const KEY_PART_INFO *keyEnd ;
   const uchar *startKeyPtr = key_ptr ;
   key_part_map startKeyPartMap = keypart_map ;
   const uchar *endKeyPtr = NULL ;
   key_part_map endKeyPartMap = 0 ;
   enum ha_rkey_function endFindFlag = HA_READ_INVALID ;
   bson::BSONObjBuilder objBuilder ;

   if ( MAX_KEY == keynr || table->s->keys <= 0 )
   {
      goto error ;
   }

   keyInfo = table->key_info + keynr ;
   if ( NULL == keyInfo || NULL == keyInfo->key_part )
   {
      goto done ;
   }

   if ( NULL != end_range )
   {
      endKeyPtr = end_range->key ;
      endKeyPartMap = end_range->keypart_map ;
      endFindFlag = end_range->flag ;
   }

   keyPart = keyInfo->key_part ;
   keyEnd = keyPart + keyInfo->user_defined_key_parts ;
   for( ; keyPart != keyEnd && (startKeyPartMap|endKeyPartMap);
        ++keyPart )
   {
      bson::BSONObj tmp_obj ;
      switch( keyPart->field->type() )
      {
         case MYSQL_TYPE_TINY:
         case MYSQL_TYPE_SHORT:
         case MYSQL_TYPE_LONG:
         case MYSQL_TYPE_INT24:
         case MYSQL_TYPE_LONGLONG:
            {
               get_int_key_range_obj( startKeyPtr,
                                      startKeyPartMap,
                                      find_flag,
                                      endKeyPtr,
                                      endKeyPartMap,
                                      endFindFlag,
                                      keyPart, tmp_obj ) ;
               break ;
            }
         case MYSQL_TYPE_FLOAT:
         case MYSQL_TYPE_DOUBLE:
            {
               get_float_key_range_obj( startKeyPtr,
                                        startKeyPartMap,
                                        find_flag,
                                        endKeyPtr,
                                        endKeyPartMap,
                                        endFindFlag,
                                        keyPart, tmp_obj ) ;
               break ;
            }
         case MYSQL_TYPE_VARCHAR:
         case MYSQL_TYPE_STRING:
         case MYSQL_TYPE_VAR_STRING:
         case MYSQL_TYPE_TINY_BLOB:
         case MYSQL_TYPE_MEDIUM_BLOB:
         case MYSQL_TYPE_LONG_BLOB:
         case MYSQL_TYPE_BLOB:
            {
               if ( !keyPart->field->binary() )
               {
                    get_text_key_range_obj(startKeyPtr,
                                          startKeyPartMap,
                                          find_flag,
                                          endKeyPtr,
                                          endKeyPartMap,
                                          endFindFlag,
                                          keyPart, tmp_obj ) ;
               }
               else
               {
                  //TODO: process the binary
                  rc = HA_ERR_UNSUPPORTED ;
               }
               break ;
            }
         default:
            rc = HA_ERR_UNSUPPORTED ;
            break ;
      }
      if ( !tmp_obj.isEmpty() )
      {
         objBuilder.append( keyPart->field->field_name,
                            tmp_obj ) ;
      }
      startKeyPtr += keyPart->store_length ;
      endKeyPtr += keyPart->store_length ;
      startKeyPartMap >>= 1 ;
      endKeyPartMap >>= 1 ;
   }
   matchObj = objBuilder.obj() ;

done:
   return rc ;
error:
   goto done ;
}
