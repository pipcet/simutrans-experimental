/* A very light 32 or less element list
 * using a fixed sized array with a 
 * head and tail node. Template type.
 *
 * Author: jamespetts. Released under the terms
 * of the Artistic Licence (for use with Simutrans),
 * and also the GPL (v 2.0). 
 * 
 * (To use other than in simutrans, remove the
 * include files and replace Simutrans specific
 * types (such as uint8) with standard types
 * (such as char). 
 */ 

#ifndef TPL_FIXED_LIST_H
#define TPL_FIXED_LIST_H

#ifndef ITERATE
#define ITERATE(collection,enumerator) for(uint32 enumerator = 0; enumerator < (collection).get_count(); enumerator++)
#endif

#ifndef ITERATE_PTR
#define ITERATE_PTR(collection,enumerator) for(uint32 enumerator = 0; enumerator < (collection)->get_count(); enumerator++)
#endif 

#include <typeinfo>
#include "../simtypes.h"
#include "../simdebug.h"

template<class T, int N> class fixed_list_tpl
{

public:

	fixed_list_tpl() : size(0), head(0), tail(0)/*, placeholder(0), placeholder_set(false)*/   { }
	
	T get_element(uint8 e)
	{
		//Depracated, retained for backwards compatibility.
		//Use [] instead.
		return (*this)[e];
	}

	T operator[](uint8 e)
	{
		uint8 i;
		if(e >= size)
		{
			dbg->fatal("fixed_list_tpl<T>::[]", "index out of bounds: %i not in 0..%d", e, size - 1);
		}
		else
		{
			i = add_index(head, e, N);
		}
		return data[i];
	}

	const T operator[](uint8 e) const
	{
		uint8 i;
		if(e >= size)
		{
			dbg->fatal("fixed_list_tpl<T>::[]", "index out of bounds: %i not in 0..%d", e, size - 1);
		}
		else
		{
			i = add_index(head, e, N);
		}
		return data[i];
	}

	void clear()
	{
		size = 0;
		head = 0;
		tail = 0;
		/*placeholder = 0;
		placeholder_set = false;*/
	}

	void trim_from_head(uint8 trim_by)
	{
		if(trim_by < 1)
		{
			return;
		}
		if(size <= trim_by)
		{
			//If trimming by more than the size,
			//this is equivalent to clearing.
			clear();
		}
		else
		{
			if(size > 1)
			{
				head = add_index(head, trim_by, N);
				size -= trim_by;
				/*if(!index_is_in_range(placeholder))
				{
					//Placeholder has been trimmed
					placeholder_set = false;
					placeholder = 0;
				}*/
			}	
			else
			{
				clear();
			}	
		}
		return;
	}

	T remove_first()
	{
		T tmp = get_element(0);
		trim_from_head(1);
		return tmp;
	}

	void trim_from_tail(uint8 trim_by)
	{
		if(trim_by < 1)
		{
			return;
		}
		if(size <= trim_by)
		{
			clear();
		}
		else
		{
			if(size > 1)
			{
				tail = subtract_index(tail, trim_by, N);
				size -= trim_by;
				/*if(!index_is_in_range(placeholder))
				{
					//Placeholder has been trimmed
					placeholder_set = false;
					placeholder = 0;
				}*/
			}
			else
			{
				clear();
			}			
		}
		return;
	}

	uint8 get_count() const
	{
		return size;
	}

	void add_to_head(T datum)
	{
		if(size > 0)
		{
			head = subtract_index(head, 1, N);
		}
		else 
		{
			head = 0;
		}
		data[head] = datum;
		if(tail == head && size > 0)
		{
			tail = subtract_index(head, 1, N);
		}
		else
		{
			size ++;
		}
		/*if(placeholder_set && placeholder == head)
		{
			//Placeholder is overwritten		
			placeholder_set = false;
			placeholder = 0;
		}*/
	}

	void add_to_tail(T datum)
	{
		if(size == 0)
		{
			tail = 0;
		}
		else
		{
			tail = add_index(tail, 1, N);
		}
		data[tail] = datum;
		if(head == tail && size > 0)
		{
			head = add_index(tail, 1, N);
			/*if(placeholder_set && placeholder == tail)
			{
				//Placeholder is overwritten		
				placeholder_set = false;
				placeholder = 0;
			}*/
		}
		else
		{
			size ++;
		}
	}

	bool add_to_head_no_overwite(T datum)
	{
		if(size < N)
		{
			add_to_head(datum);
			return true;
		}
		else
		{
			return false;
		}
	}

	bool add_to_tail_no_overwrite(T datum)
	{
		if(size < N)
		{
			add_to_tail(datum);
			return true;
		}
		else
		{
			return false;
		}
	}

	/* I hope this is unused */
	uint8 get_index_of_unused(T datum)
	{
		uint8 tmp = 255;

		for(uint8 i = 0; i < N; i++)
		{
			if(data[i] == datum && index_is_in_range(i))
			{
				tmp = i;
				break;
			}
		}

		if(tmp > (N - 1))
		{
			return 255;
		}
		else
		{
			uint8 index = subtract_index(tmp, head, N);
			return index;
		}
	}

	/*bool set_placeholder(uint8 p)
	{
		if(p >= N)
		{
			return false;
		}
		uint8 position;
		if(tail >= head)
		{
			position = add_index(p, head, N);
			if(!index_is_in_range(position))
			{
				return false;
			}
		}
		else
		{
			position = add_index(p, tail, N);
			if (!index_is_in_range(position))
			{
				return false;
			}
		}
		placeholder = position;
		placeholder_set = true;
		return true;
	}*/

	/*uint8 get_placeholder()
	{
		if(placeholder_set && index_is_in_range(placeholder))
		{
			uint8 position;
			if(tail >=head)
			{
				position = subtract_index(placeholder, head, N);
			}
			else
			{
				position = subtract_index(placeholder, tail, N);
			}
			return position;
		}

		else
		{
			return NULL;
		}
	}*/

	/*bool is_placeholder()
	{
		if(index_is_in_range(placeholder))
		{
			// Safeguarding against corruption of placeholder data.
			return placeholder_set;
		}
		else
		{
			return false;
		}
	}*/

	/*void clear_placeholder()
	{
		placeholder_set = false;
		placeholder = 0;
	}

	void placeholder_increment()
	{
		if(placeholder_set)
		{
			uint8 position = add_index(placeholder, 1, N);
			if(!index_is_in_range(position))
			{
				//Placeholder has fallen off the end.
				placeholder_set = false;
				placeholder = 0;
				return;
			}
			placeholder = position;
		}
	}

	void placeholder_decrement()
	{
		if(placeholder_set)
		{
			uint8 position = subtract_index(placeholder, 1, N);
			if(!index_is_in_range(position))
			{
				//Placeholder has fallen off the end.
				placeholder_set = false;
				placeholder = 0;
				return;
			}
			placeholder = position;
		}
	}

	T get_placeholder_element()
	{
		if(placeholder_set)
		{
			return data[placeholder];
		}
		else
		{
			return NULL;
		}
	}*/

private:

	T data[N];
	
	uint8 size;

	uint8 head;

	uint8 tail;

	//uint8 placeholder;

	//bool placeholder_set;

	//These methods are used for automating looping arithmetic

	inline uint8 add_index(uint8 base, uint8 addition, int index)
	{		
		return (addition < index ? (base + addition) % index : -1);
	}

	inline uint8 subtract_index(uint8 base, uint8 subtraction, int index)
	{
		return (subtraction < index ? (base + index - subtraction) % index : -1);
	}

	inline bool index_is_in_range(uint8 index)
	{
		//Checks whether a possible data[index] value is within the current active range
		if(tail >= head && index >= head && index <= tail)
		{
			return true;
		}
		else if(tail < head && (index >= head || index <= tail))
		{
			return true;
		}
		else
		{
			return false;
		}
	}
};
#endif
