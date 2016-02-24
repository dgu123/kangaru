#pragma once

#include "detail/function_traits.hpp"
#include "detail/traits.hpp"
#include "detail/utils.hpp"
#include "detail/container_service.hpp"
#include "detail/invoke.hpp"

#include <unordered_map>
#include <memory>
#include <type_traits>
#include <tuple>
#include <algorithm>
#include <vector>

namespace kgr {

struct ForkService;

struct Container {
private:
	template<typename Condition, typename T = int> using enable_if = detail::enable_if_t<Condition::value, T>;
	template<typename Condition, typename T = int> using disable_if = detail::enable_if_t<!Condition::value, T>;
	template<typename T> using decay = typename std::decay<T>::type;
	template<typename T> using is_single = std::is_base_of<Single, decay<T>>;
	template<typename T> using is_container_service = std::integral_constant<bool, std::is_same<T, ContainerService>::value || std::is_same<T, ForkService>::value>;
	template<typename T> using is_base_of_container_service = std::is_base_of<detail::ContainerServiceBase, T>;
	template<typename T> using parent_types = typename decay<T>::ParentTypes;
	template<int S, typename T> using tuple_element_t = typename std::tuple_element<S, T>::type;
	template<typename Tuple, int n> using tuple_seq_minus = typename detail::seq_gen<std::tuple_size<Tuple>::value - n>::type;
	template<typename T> using instance_ptr = std::unique_ptr<T, void(*)(void*)>;
	template<template<typename> class Map, typename T> using service_map_t = typename Map<T>::Service;
	template<typename T> using is_invoke_call = typename std::is_base_of<detail::InvokeCallTag, T>::type;
	template<typename T> using has_autocall = typename std::is_base_of<detail::AutocallTag, T>::type;
	using instance_cont = std::vector<instance_ptr<void>>;
	using service_cont = std::unordered_map<detail::type_id_t, void*>;
	
	template<typename T>
	static void deleter(void* i) {
		delete static_cast<T*>(i);
	}
	
	template<typename T, typename C = T, typename... Args, enable_if<std::is_constructible<T, Args...>> = 0>
	static instance_ptr<C> makeInstancePtr(Args&&... args) {
		return instance_ptr<T>{new T(std::forward<Args>(args)...), &Container::deleter<C>};
	}

	template<typename T, typename C = T, typename... Args, disable_if<std::is_constructible<T, Args...>> = 0>
	static instance_ptr<C> makeInstancePtr(Args&&... args) {
		return instance_ptr<T>{new T{std::forward<Args>(args)...}, &Container::deleter<C>};
	}
	
public:
	Container() = default;
	Container(const Container &) = delete;
	Container& operator=(const Container &) = delete;
	Container(Container&&) = default;
	Container& operator=(Container&&) = default;
	virtual ~Container() = default;
	
	template<typename T, enable_if<is_single<decay<T>>> = 0>
	void instance(T&& service) {
		save_instance(std::forward<T>(service));
	}
	
	template<typename T, typename... Args, enable_if<is_single<T>> = 0>
	void instance(Args&& ...args) {
		save_new_instance<T>(std::forward<Args>(args)...);
	}
	
	template<typename T, typename... Args>
	ServiceType<T> service(Args&& ...args) {
		return get_service<T>(std::forward<Args>(args)...).forward();
	}
	
	template<template<typename> class Map, typename U, typename ...Args>
	detail::function_result_t<decay<U>> invoke(U&& function, Args&&... args) {
		return invoke<Map>(tuple_seq_minus<detail::function_arguments_t<decay<U>>, sizeof...(Args)>{}, std::forward<U>(function), std::forward<Args>(args)...);
	}
	
	template<typename... Services, typename U, typename ...Args>
	detail::function_result_t<decay<U>> invoke(U&& function, Args&&... args) {
		return std::forward<U>(function)(service<Services>()..., std::forward<Args>(args)...);
	}
	
	inline void clear() {
		_instances.clear();
		_services.clear();
	}
	
	inline Container fork() {
		return Container{_services};
	}
	
	inline void merge(Container&& other) {
		_instances.insert(_instances.end(), std::make_move_iterator(other._instances.begin()), std::make_move_iterator(other._instances.end()));
		_services.insert(other._services.begin(), other._services.end());
	}
	
private:
	Container(service_cont& services) : _services{services} {};
	
	///////////////////////
	//   save instance   //
	///////////////////////
	
	template<typename T, typename... Args, enable_if<is_single<T>> = 0, disable_if<std::is_abstract<T>> = 0>
	T& save_new_instance(Args&&... args) {
		return save_instance(make_service_instance<T>(std::forward<Args>(args)...));
	}
	
	template<typename T, typename... Args, enable_if<is_single<T>> = 0, enable_if<std::is_abstract<T>> = 0>
	T& save_new_instance(Args&&...) {
		throw std::out_of_range{"No instance found for the requested abstract service"};
	}
	
	template<typename T, enable_if<detail::has_overrides<decay<T>>> = 0>
	T& save_instance(T&& service) {
		return save_instance(std::forward<T>(service), detail::tuple_seq<parent_types<T>>{});
	}
	
	template<typename T, disable_if<detail::has_overrides<decay<T>>> = 0>
	T& save_instance(T&& service) {
		using U = decay<T>;
		return save_instance_helper<U>(makeInstancePtr<U>(std::move(service)));
	}
	
	template<typename T, int... S>
	T& save_instance(T&& service, detail::seq<S...>) {
		using U = decay<T>;
		return save_instance_helper<U, tuple_element_t<S, parent_types<U>>...>(makeInstancePtr<U>(std::move(service)));
	}
	
	template<typename T>
	T& save_instance_helper(instance_ptr<T> service) {
		auto& serviceRef = *service;
		
		_services[detail::type_id<T>] = service.get();
		_instances.emplace_back(std::move(service));
		
		return serviceRef;
	}
	
	template<typename T, typename Override, typename... Others>
	T& save_instance_helper(instance_ptr<T> service) {
		auto overrideService = makeInstancePtr<detail::ServiceOverride<T, Override>, Override>(*service);

		_services[detail::type_id<Override>] = overrideService.get();
		_instances.emplace_back(std::move(overrideService));
		
		return save_instance_helper<T, Others...>(std::move(service));
	}
	
	///////////////////////
	//    get service    //
	///////////////////////
	
	template<typename T, typename... Args, disable_if<is_single<T>> = 0, disable_if<is_base_of_container_service<T>> = 0>
	T get_service(Args&&... args) {
		return make_service_instance<T>(std::forward<Args>(args)...);
	}
	
	template<typename T, enable_if<is_container_service<T>> = 0>
	T get_service() {
		return T{*this};
	}
	
	template<typename T, enable_if<is_base_of_container_service<T>> = 0, disable_if<is_container_service<T>> = 0>
	T get_service() {
		return T{*dynamic_cast<typename T::Type*>(this)};
	}
	
	template<typename T, enable_if<is_single<T>> = 0, disable_if<is_base_of_container_service<T>> = 0>
	T& get_service() {
		if (auto&& service = _services[detail::type_id<T>]) {
			return *static_cast<T*>(service);
		} else {
			return save_new_instance<T>();
		}
	}
	
	///////////////////////
	//   make instance   //
	///////////////////////
	
	template<typename T, typename... Args>
	T make_service_instance(Args&&... args) {
		T service = invoke_raw(&T::construct, std::forward<Args>(args)...);
		invoke_service(service);
		return service;
	}
	
	///////////////////////
	//      invoke       //
	///////////////////////
	
	template<typename U, typename ...Args, int... S>
	detail::function_result_t<decay<U>> invoke(detail::seq<S...>, U&& function, Args&&... args) {
		return std::forward<U>(function)(get_service<decay<detail::function_argument_t<S, decay<U>>>>()..., std::forward<Args>(args)...);
	}
	
	template<template<typename> class Map, typename U, typename ...Args, int... S>
	detail::function_result_t<decay<U>> invoke(detail::seq<S...>, U&& function, Args&&... args) {
		return std::forward<U>(function)(service<service_map_t<Map, detail::function_argument_t<S, decay<U>>>>()..., std::forward<Args>(args)...);
	}
	
	template<typename U, typename ...Args, enable_if<is_invoke_call<decay<U>>> = 0>
	detail::function_result_t<decay<U>> invoke(U&& function, Args&&... args) {
		using V = decay<U>;
		return invoke(detail::tuple_seq<typename V::Params>{}, std::forward<U>(function), std::forward<Args>(args)...);
	}
	
	template<typename U, typename ...Args, int... S, enable_if<is_invoke_call<decay<U>>> = 0>
	detail::function_result_t<decay<U>> invoke(detail::seq<S...>, U&& function, Args&&... args) {
		using V = decay<U>;
		return invoke<tuple_element_t<S, typename V::Params>...>(std::forward<U>(function), std::forward<Args>(args)...);
	}
	
	template<typename U, typename ...Args, disable_if<is_invoke_call<decay<U>>> = 0>
	detail::function_result_t<decay<U>> invoke_raw(U&& function, Args&&... args) {
		return invoke(tuple_seq_minus<detail::function_arguments_t<decay<U>>, sizeof...(Args)>{}, std::forward<U>(function), std::forward<Args>(args)...);
	}
	
	///////////////////////
	//     autocall      //
	///////////////////////
	
	// This function starts the iteration (invoke_service_helper)
	template<typename T, enable_if<detail::has_invoke<decay<T>>> = 0>
	void invoke_service(T&& service) {
		using U = decay<T>;
		invoke_service_helper<typename U::invoke>(std::forward<T>(service));
	}
	
	// This function is the iteration for invoke_service
	template<typename Method, typename T, enable_if<detail::has_next<Method>> = 0>
	void invoke_service_helper(T&& service) {
		do_invoke_service<Method>(std::forward<T>(service));
		invoke_service_helper<typename Method::Next>(std::forward<T>(service));
	}
	
	// This is the last iteration of invoke_service
	template<typename Method, typename T, disable_if<detail::has_next<Method>> = 0>
	void invoke_service_helper(T&&) {}
	
	// This function is the do_invoke_service when <Method> is kgr::Invoke with parameters explicitly listed.
	template<typename Method, typename T, enable_if<is_invoke_call<Method>> = 0>
	void do_invoke_service(T&& service) {
		do_invoke_service<Method>(detail::tuple_seq<typename Method::Params>{}, std::forward<T>(service));
	}
	
	// This function is a helper for do_invoke_service when <Method> is kgr::Invoke with parameters explicitly listed.
	template<typename Method, typename T, int... S>
	void do_invoke_service(detail::seq<S...>, T&& service) {
		using U = decay<T>;
		do_invoke_service(
			detail::tuple_seq<detail::function_arguments_t<typename Method::value_type>>{},
			std::forward<T>(service),
			&U::template autocall<Method, tuple_element_t<S, typename Method::Params>...>
		);
	}
	
	// This function is the do_invoke_service handling the new syntax for autocall
	template<typename Method, typename T, disable_if<is_invoke_call<Method>> = 0, enable_if<has_autocall<decay<T>>> = 0>
	void do_invoke_service(T&& service) {
		using U = decay<T>;
		do_invoke_service(detail::tuple_seq<std::tuple<ContainerService>>{}, std::forward<T>(service), &U::template autocall<Method, U::template Map>);
	}
	
	// This function is the do_invoke_service with the old syntax
	template<typename Method, typename T, disable_if<is_invoke_call<Method>> = 0, disable_if<has_autocall<decay<T>>> = 0>
	void do_invoke_service(T&& service) {
		do_invoke_service(detail::tuple_seq<detail::function_arguments_t<typename Method::value_type>>{}, std::forward<T>(service), Method::value);
	}
	
	// This function is the do_invoke_service that take the method to invoke as parameter.
	// It invokes the function sent as parameter.
	template<typename T, typename F, int... S>
	void do_invoke_service(detail::seq<S...>, T&& service, F&& function) {
		invoke_raw([&service, &function](detail::function_argument_t<S, decay<F>>... args){
			(std::forward<T>(service).*std::forward<F>(function))(std::forward<detail::function_argument_t<S, decay<F>>>(args)...);
		});
	}
	
	template<typename T, disable_if<detail::has_invoke<decay<T>>> = 0>
	void invoke_service(T&&) {}
	
	///////////////////////
	//     instances     //
	///////////////////////
	
	instance_cont _instances;
	service_cont _services;
};

}  // namespace kgr
